#include <boost/unordered_set.hpp>
#include <boost/algorithm/string.hpp>
#include <tuple>

#include "obmolopener.h"
#include "box.h"
#include "tee.h"

#ifndef SMINA_FLEXINFO_H
#define SMINA_FLEXINFO_H

/* Store information for identifying flexible residues in receptor and
 * provide routines for extracting these residues as needed.
 */
class FlexInfo {
    double flex_dist;
    boost::unordered_set<std::tuple<char, int, char> > residues; //chain, resid, insertion code
    OpenBabel::OBMol distligand;
    tee& log;

    char defaultch = ' '; // Default chain

  public:
    FlexInfo(tee& l)
        : flex_dist(0), log(l) {
    }
    FlexInfo(const std::string& flexres, double flexdist,
        const std::string& ligand, tee& l);
    bool hasContent() const {
      return residues.size() > 0 || flex_dist > 0;
    }

    void extractFlex(OpenBabel::OBMol& receptor, OpenBabel::OBMol& rigid,
        std::string& flexpdbqt);

    void printFlex() const;

  private:
    void sanitizeResidues(OpenBabel::OBMol& receptor); //remove inflexible residues from residues set

};

#endif /* SMINA_FLEXINFO_H */
