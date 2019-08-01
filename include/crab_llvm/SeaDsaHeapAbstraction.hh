#pragma once

#include "crab_llvm/config.h"

#ifdef HAVE_SEA_DSA

#include "crab_llvm/HeapAbstraction.hh"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/ImmutableSet.h"

// forward declarations
namespace sea_dsa {
  class GlobalAnalysis;
  class Node;
  class Cell;
}

namespace llvm {
  class DataLayout;
  class CallGraph;
  class TargetLibraryInfo;
  class AllocWrapInfo;
}

namespace crab_llvm {
  /* 
   * Wrapper for sea-dsa (https://github.com/seahorn/sea-dsa)
   */
  class SeaDsaHeapAbstraction: public HeapAbstraction {

   public:

     using typename HeapAbstraction::region_t;
     using typename HeapAbstraction::region_vector_t;

   private:

    // XXX: We should use sea_dsa::Graph::SetFactory.
    // We copy here the definition of sea_dsa::Graph::SetFactory so
    // that we don't need to include Graph.hh
    typedef llvm::ImmutableSet<llvm::Type *> Set;
    typedef typename Set::Factory SetFactory;
    typedef std::pair<region_t, bool> region_bool_t;
    
    llvm::DenseMap<const llvm::Function*, std::vector<region_t>> m_func_accessed;
    llvm::DenseMap<const llvm::Function*, std::vector<region_t>> m_func_mods;
    llvm::DenseMap<const llvm::Function*, std::vector<region_t>> m_func_news;
    llvm::DenseMap<const llvm::CallInst*, std::vector<region_t>> m_callsite_accessed;
    llvm::DenseMap<const llvm::CallInst*, std::vector<region_t>> m_callsite_mods;
    llvm::DenseMap<const llvm::CallInst*, std::vector<region_t>> m_callsite_news;

    unsigned int getId(const sea_dsa::Cell& c);

    // compute and cache the set of read, mod and new nodes of a whole
    // function such that mod nodes are a subset of the read nodes and
    // the new nodes are disjoint from mod nodes.
    void computeReadModNewNodes(const llvm::Function &f);

    // Compute and cache the set of read, mod and new nodes of a
    // callsite such that mod nodes are a subset of the read nodes and
    // the new nodes are disjoint from mod nodes.
    typedef llvm::DenseMap<const llvm::CallInst*, std::vector<region_bool_t>> callsite_map_t;   
    void computeReadModNewNodesFromCallSite(llvm::CallInst &I,
					    callsite_map_t& accessed,
					    callsite_map_t& mods,
					    callsite_map_t& news);

   public:

    SeaDsaHeapAbstraction(llvm::Module& M, llvm::CallGraph& cg,
			  const llvm::DataLayout& dl,
			  const llvm::TargetLibraryInfo& tli,
			  const sea_dsa::AllocWrapInfo& alloc_wrap_info,
			  bool is_context_sensitive,
			  bool disambiguate_unknown  = false,
			  bool disambiguate_ptr_cast = false,
			  bool disambiguate_external = false);

    ~SeaDsaHeapAbstraction();
    
    virtual region_t getRegion(const llvm::Function &F, llvm::Value *V) override;
    
    virtual const llvm::Value* getSingleton(int region) const override;
    
    virtual region_vector_t getAccessedRegions(const llvm::Function &F) override;
    
    virtual region_vector_t getOnlyReadRegions(const llvm::Function &F) override;
    
    virtual region_vector_t getModifiedRegions(const llvm::Function &F) override;
    
    virtual region_vector_t getNewRegions(const llvm::Function &F) override;
    
    virtual region_vector_t getAccessedRegions(llvm::CallInst &I) override;
    
    virtual region_vector_t getOnlyReadRegions(llvm::CallInst &I) override;
    
    virtual region_vector_t getModifiedRegions(llvm::CallInst &I) override;
    
    virtual region_vector_t getNewRegions(llvm::CallInst &I) override;
    
    virtual llvm::StringRef getName() const override {
      return "SeaDsaHeapAbstraction";
    }

  private:
    
    llvm::Module &m_m;
    const llvm::DataLayout& m_dl;
    sea_dsa::GlobalAnalysis* m_dsa;
    SetFactory* m_fac;
    /// map from Node to id
    llvm::DenseMap<const sea_dsa::Node*, unsigned> m_node_ids;
    /// reverse map
    boost::unordered_map<unsigned, const sea_dsa::Node*> m_rev_node_ids;
    unsigned m_max_id;
    bool m_disambiguate_unknown;
    bool m_disambiguate_ptr_cast;
    bool m_disambiguate_external;

  };

} // end namespace crab_llvm
#endif  /*HAVE_SEA_DSA*/
