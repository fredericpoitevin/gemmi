// Copyright 2019 Global Phasing Ltd.
//
// Searching for links based on the _chem_link table from monomer dictionary.

#ifndef GEMMI_LINKHUNT_HPP_
#define GEMMI_LINKHUNT_HPP_

#include <map>
#include <unordered_map>
#include "calculate.hpp"  // for calculate_chiral_volume
#include "elem.hpp"
#include "model.hpp"
#include "monlib.hpp"
#include "subcells.hpp"

namespace gemmi {

inline Connection* find_connection_by_cra(Model& model, CRA& cra1, CRA& cra2) {
  for (Connection& c : model.connections)
    if ((atom_matches(cra1, c.atom[0]) && atom_matches(cra2, c.atom[1])) ||
        (atom_matches(cra1, c.atom[1]) && atom_matches(cra2, c.atom[0])))
      return &c;
  return nullptr;
}

struct LinkHunt {
  struct Match {
    const ChemLink* chem_link = nullptr;
    int chem_link_count = 0;
    CRA cra1;
    CRA cra2;
    bool same_asu;
    float bond_length = 0.f;
    Connection* conn = nullptr;
  };

  double global_max_dist = 2.34; // ZN-CYS
  std::multimap<std::string, const ChemLink*> links;
  std::unordered_map<std::string, ChemLink::Group> res_group;
  std::map<std::string, double> max_dist_per_atom;

  void index_chem_links(const MonLib& monlib) {
    static const std::vector<std::string> blacklist = {
      "TRANS", "PTRANS", "NMTRANS", "CIS", "PCIS", "NMCIS", "p", "SS"
    };
    for (const auto& iter : monlib.links) {
      const ChemLink& link = iter.second;
      if (link.rt.bonds.empty())
        continue;
      if (link.rt.bonds.size() > 1)
        fprintf(stderr, "Note: considering only the first bond in %s\n",
                link.id.c_str());
      if (link.side1.comp.empty() && link.side2.comp.empty())
        if (link.side1.group == ChemLink::Group::Null ||
            link.side2.group == ChemLink::Group::Null ||
            in_vector(link.id, blacklist))
          continue;
      const Restraints::Bond& bond = link.rt.bonds[0];
      if (bond.value > global_max_dist)
        global_max_dist = bond.value;
      for (const std::string& atom_name : {bond.id1.atom, bond.id1.atom}) {
        auto r = max_dist_per_atom.emplace(atom_name, bond.value);
        if (!r.second && r.first->second < bond.value)
          r.first->second = bond.value;
      }
      links.emplace(bond.lexicographic_str(), &link);
    }
    for (const auto& ri : monlib.residue_infos) {
      ChemLink::Group group = ChemLink::Group::Null;
      if (ri.second.is_amino_acid())
        group = ChemLink::Group::Peptide;
      else if (ri.second.is_nucleic_acid())
        group = ChemLink::Group::DnaRna;
      else if (ri.second.kind == ResidueInfo::PYR)
        group = ChemLink::Group::Pyranose;
      res_group.emplace(ri.first, group);
    }
  }

  bool match_link_side(const ChemLink::Side& side,
                       const std::string& resname) const {
    if (!side.comp.empty())
      return side.comp == resname;
    if (side.group == ChemLink::Group::Null)
      return false;
    auto iter = res_group.find(resname);
    return iter != res_group.end() && iter->second == side.group;
  }

  std::vector<Match> find_possible_links(Structure& st, double bond_margin,
                                                        double radius_margin) {
    std::vector<Match> results;
    Model& model = st.models.at(0);
    SubCells sc(model, st.cell, std::max(5.0, global_max_dist * bond_margin));
    sc.populate(model);
    for (int n_ch = 0; n_ch != (int) model.chains.size(); ++n_ch) {
      Chain& chain = model.chains[n_ch];
      for (int n_res = 0; n_res != (int) chain.residues.size(); ++n_res) {
        Residue& res = chain.residues[n_res];
        for (int n_atom = 0; n_atom != (int) res.atoms.size(); ++n_atom) {
          Atom& atom = res.atoms[n_atom];
          auto max_dist = max_dist_per_atom.find(atom.name);
          if (max_dist == max_dist_per_atom.end())
            continue;
          sc.for_each(atom.pos, atom.altloc, (float) max_dist->second,
                      [&](SubCells::Mark& m, float dist_sq) {
              // do not consider connections inside a residue
              if (m.image_idx == 0 && m.chain_idx == n_ch &&
                  m.residue_idx == n_res)
                return;
              // avoid reporting connections twice (A-B and B-A)
              if (m.chain_idx < n_ch || (m.chain_idx == n_ch &&
                    (m.residue_idx < n_res || (m.residue_idx == n_res &&
                                               m.atom_idx < n_atom))))
                return;
              // atom can be linked with its image, but if the image
              // is too close the atom is likely on special position.
              if (m.chain_idx == n_ch && m.residue_idx == n_res &&
                  m.atom_idx == n_atom && dist_sq < sq(0.8f))
                return;
              CRA cra = m.to_cra(model);

              // search for a match in chem_links
              auto range = links.equal_range(Restraints::lexicographic_str(
                                                  atom.name, cra.atom->name));
              Match match;
              for (auto iter = range.first; iter != range.second; ++iter) {
                const ChemLink& link = *iter->second;
                const Restraints::Bond& bond = link.rt.bonds[0];
                if (dist_sq > sq(bond.value * bond_margin))
                  continue;
                bool order1;
                if (bond.id1.atom == atom.name &&
                    match_link_side(link.side1, res.name) &&
                    match_link_side(link.side2, cra.residue->name))
                  order1 = true;
                else if (bond.id2.atom == atom.name &&
                    match_link_side(link.side2, res.name) &&
                    match_link_side(link.side1, cra.residue->name))
                  order1 = false;
                else
                  continue;
                // check chirality
                int chirality_score = 0;
                for (const Restraints::Chirality& chirality : link.rt.chirs)
                  if (chirality.chir != ChiralityType::Both) {
                    Residue& res1 = order1 ? res : *cra.residue;
                    Residue* res2 = order1 ? cra.residue : &res;
                    char alt = atom.altloc ? atom.altloc : cra.atom->altloc;
                    Atom* at1 = chirality.id_ctr.get_from(res1, res2, alt);
                    Atom* at2 = chirality.id1.get_from(res1, res2, alt);
                    Atom* at3 = chirality.id2.get_from(res1, res2, alt);
                    Atom* at4 = chirality.id3.get_from(res1, res2, alt);
                    if (at1 && at2 && at3 && at4) {
                      double vol = calculate_chiral_volume(at1->pos, at2->pos,
                                                           at3->pos, at4->pos);
                      if (chirality.is_wrong(vol))
                        --chirality_score;
                    }
                  }
                if (chirality_score < 0)
                  continue;
                //if (match.chem_link)
                //  printf("DEBUG: %s %s (%d)\n", match.chem_link->id.c_str(),
                //         link.id.c_str(), match.chem_link_count);
                match.chem_link = &link;
                match.chem_link_count++;
                if (order1) {
                  match.cra1 = {&chain, &res, &atom};
                  match.cra2 = cra;
                } else {
                  match.cra1 = cra;
                  match.cra2 = {&chain, &res, &atom};
                }
              }

              // potential other links according to covalent radii
              if (!match.chem_link) {
                float r1 = atom.element.covalent_r();
                float r2 = cra.atom->element.covalent_r();
                if (dist_sq > sq((r1 + r2) * radius_margin))
                  return;
                match.cra1 = cra;
                match.cra2 = {&chain, &res, &atom};
              }

              match.same_asu = !m.image_idx;
              match.bond_length = std::sqrt(dist_sq);
              results.push_back(match);
          });
        }
      }
    }
    for (Match& match : results)
      match.conn = find_connection_by_cra(model, match.cra1, match.cra2);
    return results;
  }
};

} // namespace gemmi
#endif
// vim:sw=2:ts=2:et
