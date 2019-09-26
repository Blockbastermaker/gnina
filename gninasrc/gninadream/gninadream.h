#pragma once
#include "loss.h"
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/text_format.h>
#include "cnn_scorer.h"
#include "tee.h"
#include "molgetter.h"
#include <boost/algorithm/string.hpp>

using namespace caffe;
typedef MolGridDataLayer<float> mgridT;

void setLigand(model& m, std::vector<float3>& coords, std::vector<smt>& smtypes) {
  // Ligand atoms start at ligand root node start idx
  sz n = m.m_num_movable_atoms - m.ligands[0].node.begin;

  auto m_atoms = m.get_movable_atoms().cbegin() + m.ligands[0].node.begin;
  auto m_coords = m.coordinates().cbegin() + m.ligands[0].node.begin;

  for(sz i=0;  i<n; ++i) {
    smtypes.push_back(m_atoms[i].sm);
    const vec& coord = m_coords[i];
    coords.push_back(gfloat3(coord[0], coord[1], coord[2]));
  }
}

void setReceptor(model& m, std::vector<float3>& coords, std::vector<smt>& smtypes) {
  auto& at = m.get_fixed_atoms();
  sz n = at.size();
  for (sz i=0; i<n; ++i) {
    const vec& coord = at[i].coords;
    coords.push_back(make_float3(coord[0], coord[1], coord[2]));
    smtypes.push_back(at[i].sm);
  }
}

void do_exact_vs(LayerParameter param, caffe::Net<float>& net, 
    std::string vsfile, std::vector<std::string>& ref_ligs,
    std::vector<caffe::shared_ptr<std::ostream> >& out, 
    bool gpu, std::string dist_method, float positive_threshold, float negative_threshold, 
    bool compute_cost=true) {
  // use net top blob to do virtual screen against input sdf
  // produce output file for each input from which we started optimization
  // output will just be overlap score, in order of the compounds in the
  // original file
  // right now we assume these are pre-generated poses, although we could dock
  // them internally or generate conformers in theory. 
  //
  // reinit MolGrid with params for virtual screening
  // for each example rec is the lig used to set center (unless there was no
  // lig, in which case we effectively fix center to origin) and lig is one of the
  // vs ligands; we set use_rec_center and ignore_rec if there was an autocenter
  // lig. this is annoying because done the naive way we have to regrid the
  // same ligand many times
  unsigned nopts = net.top_vecs()[0][0]->shape()[0];
  unsigned batch_size = 1; // inmem for now

  tee log(true);
  unsigned subgrid_dim = 4; // right now require grid dimension be divisible by this if using EMD
  FlexInfo finfo(log);
  MolGetter mols(std::string(), std::string(), finfo, false, false, log);
  mols.setInputFile(vsfile);

  MolGridDataParameter* mparam = param.mutable_molgrid_data_param();
  if (!mparam) {
    std::cerr << "Virtual screen passed non-molgrid layer parameter.\n";
    std::exit(1);
  }
  mparam->set_ignore_rec(true);
  mparam->set_ignore_ligand(false);
  mparam->set_has_affinity(false);
  mparam->set_inmemory(true);
  mparam->set_use_rec_center(true);
  mparam->set_batch_size(batch_size);
  // initblobs for virtual screen compound grids and set up
  vector<Blob<float>*> bottom; // will always be empty
  vector<Blob<float>*> top(2); // want to use MGrid::Forward so we'll need a dummy labels blob
  Blob<float> datablob;
  Blob<float> labelsblob;
  top[0] = &datablob;
  top[1] = &labelsblob;

  mgridT opt_mgrid(param);
  opt_mgrid.DataLayerSetUp(bottom, top);
  // actually want size for lig channels only
  unsigned example_size = opt_mgrid.getExampleSize();
  unsigned ligGridSize = opt_mgrid.getNumGridPoints() * opt_mgrid.getNumLigandTypes();
  unsigned recGridSize = opt_mgrid.getNumGridPoints()* opt_mgrid.getNumReceptorTypes();
  float* gpu_score = nullptr;
  // two floats since for the sum method we need two storage locations
  if (gpu) 
    CUDA_CHECK_GNINA(cudaMalloc(&gpu_score, sizeof(float)*2));
  
  // if using EMD for virtual screen, populate cost_matrix once
  flmat cost_matrix;
  unsigned blocks_per_side;
  unsigned ncubes;
  unsigned ntypes;
  unsigned n_matrix_elems;
  unsigned dim;
  if (!std::strcmp(dist_method.c_str(), "emd") && compute_cost) {
    double dimension = opt_mgrid.getDimension();
    dim = dimension * 2 + 1;
    if (dim % subgrid_dim) {
      std::cerr << "Grid dimension not divisible by 4, which is currently required for EMD.\n";
      std::exit(1);
    }
    blocks_per_side = dim / subgrid_dim;
    ncubes = blocks_per_side * blocks_per_side * blocks_per_side;
    ntypes = opt_mgrid.getNumLigandTypes();
    n_matrix_elems = ncubes * ntypes;
    cost_matrix = flmat(n_matrix_elems, 0);
    populate_cost_matrix(ncubes, subgrid_dim, ntypes, blocks_per_side, dimension, cost_matrix);
  }

  //VS compounds are currently done one at a time, inmem
  //out is the vector of output filestreams, one per optimized input to be
  //screened against 
  model m;
  for (size_t i=0; i<out.size(); ++i) {
    std::vector<float> scores;
    unsigned offset = i * example_size + recGridSize;
    std::string& next_ref_lig = ref_ligs[i];
    bool is_gninatypes = boost::algorithm::ends_with(next_ref_lig, ".gninatypes");
    
    if (next_ref_lig != "none" && !is_gninatypes)
      mols.create_init_model(ref_ligs[0], std::string(), finfo, log);
    else
      mols.create_init_model(std::string(), std::string(), finfo, log);
    for (;;) {
      if (!mols.readMoleculeIntoModel(m)) {
        break;
      }
      unsigned natoms = m.num_movable_atoms();
      std::vector<smt> ligand_smtypes;
      std::vector<float3> ligand_coords;
      setLigand(m, ligand_coords, ligand_smtypes);
      opt_mgrid.setLigand(ligand_coords, ligand_smtypes);
      if (next_ref_lig != "none") {
        if (is_gninatypes) {
          struct info {
            float x,y,z;
            int type;
          } gt_atom;
          std::vector<float3> rec_coords;
          std::vector<smt> rec_smtypes;

          std::ifstream in(next_ref_lig.c_str());
          CHECK(in) << "Could not read " << next_ref_lig;

          while(in.read((char*)&gt_atom, sizeof(gt_atom)))
          {
            smt t = (smt)gt_atom.type;
            rec_smtypes.push_back(t);
            rec_coords.push_back(make_float3(gt_atom.x, gt_atom.y, gt_atom.z));

          }
          opt_mgrid.setReceptor(rec_coords, rec_smtypes);
        }
        else {
          std::vector<smt> rec_smtypes;
          std::vector<float3> rec_coords;
          setReceptor(m, rec_coords, rec_smtypes);
          opt_mgrid.setReceptor(rec_coords, rec_smtypes);
        }
      }
      else
        opt_mgrid.setGridCenter(vec(0, 0, 0));
      opt_mgrid.setLabels(1, 10); 
      if (gpu) {
        opt_mgrid.Forward_gpu(bottom, top);
        const float* optgrid = net.top_vecs()[0][0]->gpu_data();
        const float* screengrid = top[0]->gpu_data();
        CUDA_CHECK_GNINA(cudaMemset(gpu_score, 0, sizeof(float)*2));
        if (!std::strcmp(dist_method.c_str(), "l1"))
          do_gpu_l1(optgrid + offset, screengrid + recGridSize, gpu_score, ligGridSize);
        else if (!std::strcmp(dist_method.c_str(), "l2"))
          do_gpu_l2sq(optgrid + offset, screengrid + recGridSize, gpu_score, ligGridSize);
        else if(!std::strcmp(dist_method.c_str(), "mult"))
          do_gpu_mult(optgrid + offset, screengrid + recGridSize, gpu_score, ligGridSize);
        else if(!std::strcmp(dist_method.c_str(), "sum")) {
          float l2;
          float mult;
          do_gpu_l2sq(optgrid + offset, screengrid + recGridSize, gpu_score, ligGridSize);
          CUDA_CHECK_GNINA(cudaMemcpy(&l2, gpu_score, sizeof(float), cudaMemcpyDeviceToHost));
          do_gpu_mult(optgrid + offset, screengrid + recGridSize, gpu_score+1, ligGridSize);
          CUDA_CHECK_GNINA(cudaMemcpy(&mult, gpu_score+1, sizeof(float), cudaMemcpyDeviceToHost));
          l2 = std::sqrt(l2);
          scores.push_back((l2/100.f + mult) / (natoms * ligGridSize));
        }
        else if(!std::strcmp(dist_method.c_str(), "threshold")) {
          do_gpu_thresh(optgrid + offset, screengrid + recGridSize, gpu_score, ligGridSize,
              positive_threshold, negative_threshold);
        }
        else if (!std::strcmp(dist_method.c_str(), "emd")) {
          do_gpu_emd(optgrid + offset, screengrid + recGridSize, gpu_score, dim, subgrid_dim, 
              blocks_per_side, ntypes, ligGridSize, cost_matrix);
        }
        else {
          std::cerr << "Unknown distance method for overlap-based virtual screen\n";
          exit(-1);
        }
        if(std::strcmp(dist_method.c_str(), "sum")) {
          float scoresq;
          CUDA_CHECK_GNINA(cudaMemcpy(&scoresq, gpu_score, sizeof(float), cudaMemcpyDeviceToHost));
          if (!std::strcmp(dist_method.c_str(), "l2"))
            scoresq = std::sqrt(scoresq);
          scores.push_back(scoresq / (natoms * ligGridSize));
        }
      }
      else {
        opt_mgrid.Forward_cpu(bottom, top);
        const float* optgrid = net.top_vecs()[0][0]->cpu_data();
        const float* screengrid = top[0]->cpu_data();
        scores.push_back(float());
        if (!std::strcmp(dist_method.c_str(), "l1"))
          cpu_l1(optgrid + offset, screengrid + recGridSize, &scores.back(), 
              ligGridSize);
        else if (!std::strcmp(dist_method.c_str(), "l2"))
          cpu_l2sq(optgrid + offset, screengrid + recGridSize, &scores.back(), 
              ligGridSize);
        else if(!std::strcmp(dist_method.c_str(), "mult"))
          cpu_mult(optgrid + offset, screengrid + recGridSize, &scores.back(), 
              ligGridSize);
        else if(!std::strcmp(dist_method.c_str(), "sum")) {
          float l2;
          cpu_l2sq(optgrid + offset, screengrid + recGridSize, &l2, 
              ligGridSize);
          float mult;
          cpu_mult(optgrid + offset, screengrid + recGridSize, &mult, 
              ligGridSize);
          scores.back() = (l2/100.f + mult) / (natoms * ligGridSize);
        }
        else if(!std::strcmp(dist_method.c_str(), "threshold")) {
          cpu_thresh(optgrid + offset, screengrid + recGridSize, &scores.back(), 
              ligGridSize, positive_threshold, negative_threshold);
        }
        else if (!std::strcmp(dist_method.c_str(), "emd")) {
          do_cpu_emd(optgrid+offset, screengrid + recGridSize, &scores.back(), dim, subgrid_dim, 
              blocks_per_side, ntypes, ligGridSize, cost_matrix);
        }
        else {
          std::cerr << "Unknown distance method for overlap-based virtual screen\n";
          exit(-1);
        }
        scores.back() = std::sqrt(scores.back()) / (natoms * ligGridSize);
      }
    }
    // write to output
    for (auto& score : scores) {
      *out[i] << score << "\n";
    }
  }
}

void do_approx_vs(mgridT* opt_mgrid, caffe::Net<float>& net, 
    std::string vsfile, std::vector<std::string>& ref_ligs,std::vector<std::ostream>& out, 
    bool gpu) {
  // TODO?
  assert(0);
}

void do_constant_fill(float* fillgrid, size_t gsize, float fillval);
