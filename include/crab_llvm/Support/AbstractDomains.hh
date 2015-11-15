#ifndef __ABSTRACT_DOMAINS_HH_
#define __ABSTRACT_DOMAINS_HH_

/* Common header files and IO support for abstract domains */

#include <crab/domains/linear_constraints.hpp>                      
#include <crab/domains/intervals.hpp>                      
#include <crab/domains/intervals_congruences.hpp>                      
#include <crab/domains/dbm.hpp>
#include <crab/domains/split_dbm.hpp>
#include <crab/domains/var_packing_naive_dbm.hpp>
#include <crab/domains/apron_domains.hpp>
#include <crab/domains/term_equiv.hpp>
#include <crab/domains/boxes.hpp>
#include <crab/domains/array_smashing.hpp> 

namespace llvm
{

  using namespace std;

  template <typename Number, typename VariableName>
  inline llvm::raw_ostream& operator<< (llvm::raw_ostream& o, 
                                        ikos::linear_expression<Number,VariableName>& e)
  {
    ostringstream s;
    s << e;
    o << s.str ();
    return o;
  }


  template <typename Number, typename VariableName>
  inline llvm::raw_ostream& operator<< (llvm::raw_ostream& o, 
                                        ikos::linear_constraint<Number,VariableName>& cst)
  {
    ostringstream s;
    s << cst;
    o << s.str ();
    return o;
  }

  template <typename Number, typename VariableName>
  inline llvm::raw_ostream& operator<< (llvm::raw_ostream& o, 
                                        ikos::linear_constraint_system<Number,VariableName>& csts)
  {
    ostringstream s;
    s << csts;
    o << s.str ();
    return o;
  }

  template <typename Number, typename VariableName>
  inline llvm::raw_ostream& operator<< (llvm::raw_ostream& o, 
                                        ikos::interval_domain<Number,VariableName>& inv)
  {
    ostringstream s;
    s << inv;
    o << s.str ();
    return o;
  }
  
  template <typename Number, typename VariableName>
  inline llvm::raw_ostream& operator<< (llvm::raw_ostream& o, 
                                        ikos::interval_congruence_domain<Number,VariableName>& inv)
  {
    ostringstream s;
    s << inv;
    o << s.str ();
    return o;
  }

  template <typename Number, typename VariableName>
  inline llvm::raw_ostream& operator<< (llvm::raw_ostream& o, 
                                        crab::domains::DBM<Number,VariableName>& inv)
  {
    ostringstream s;
    s << inv;
    o << s.str ();
    return o;
  }

  template <typename Number, typename VariableName>
  inline llvm::raw_ostream& operator<< (llvm::raw_ostream& o, 
                                        crab::domains::SplitDBM<Number,VariableName>& inv)
  {
    ostringstream s;
    s << inv;
    o << s.str ();
    return o;
  }

  template <typename Number, typename VariableName>
  inline llvm::raw_ostream& operator<< (llvm::raw_ostream& o, 
                                        ikos::var_packing_naive_dbm<Number,VariableName>& inv)
  {
    ostringstream s;
    s << inv;
    o << s.str ();
    return o;
  }

  template <typename Number, typename VariableName>
  inline llvm::raw_ostream& operator<< (llvm::raw_ostream& o, 
                                        crab::domains::apron_domain
                                        <Number,VariableName, 
                                        crab::domains::apron_domain_id_t::APRON_INT>& inv)
  {
    ostringstream s;
    s << inv;
    o << s.str ();
    return o;
  }

  template <typename Number, typename VariableName>
  inline llvm::raw_ostream& operator<< (llvm::raw_ostream& o, 
                                        crab::domains::apron_domain
                                        <Number,VariableName, 
                                        crab::domains::apron_domain_id_t::APRON_OCT>& inv)
  {
    ostringstream s;
    s << inv;
    o << s.str ();
    return o;
  }

  template <typename Number, typename VariableName>
  inline llvm::raw_ostream& operator<< (llvm::raw_ostream& o, 
                                        crab::domains::apron_domain
                                        <Number,VariableName, 
                                        crab::domains::apron_domain_id_t::APRON_OPT_OCT>& inv)
  {
    ostringstream s;
    s << inv;
    o << s.str ();
    return o;
  }

  template <typename Number, typename VariableName>
  inline llvm::raw_ostream& operator<< (llvm::raw_ostream& o, 
                                        crab::domains::apron_domain
                                        <Number,VariableName, 
                                        crab::domains::apron_domain_id_t::APRON_PK>& inv)
  {
    ostringstream s;
    s << inv;
    o << s.str ();
    return o;
  }

  template <typename Number, typename VariableName>
  inline llvm::raw_ostream& operator<< (llvm::raw_ostream& o, 
                                        crab::domains::boxes_domain<Number,VariableName>& inv)
  {
    ostringstream s;
    s << inv;
    o << s.str ();
    return o;
  }

  template <typename Base, typename Number, typename VariableName>
  inline llvm::raw_ostream& operator<< (llvm::raw_ostream& o, 
                                        crab::domains::array_smashing
                                        <Base, Number, VariableName> & inv)
  {
    ostringstream s;
    s << inv;
    o << s.str ();
    return o;
  }
} // end namespace
#endif 
