/**
 * @file    computeMap.hpp
 * @brief   implements the sequence mapping logic
 * @author  Chirag Jain <cjain7@gatech.edu>
 */

#ifndef SKETCH_MAP_HPP
#define SKETCH_MAP_HPP

#include <iterator>
#include <limits>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <fstream>
#include <zlib.h>
#include <cassert>
#include <numeric>
#include <iostream>
#include <filesystem>
namespace fs = std::filesystem;
#include <queue>
#include <algorithm>
#include <unordered_set>

//Own includes
#include "map/include/base_types.hpp"
#include "map/include/map_parameters.hpp"
#include "map/include/commonFunc.hpp"
#include "map/include/winSketch.hpp"
#include "map/include/map_stats.hpp"
#include "map/include/slidingMap.hpp"
#include "map/include/MIIteratorL2.hpp"
#include "map/include/ThreadPool.hpp"
#include "map/include/filter.hpp"

//External includes
#include "common/seqiter.hpp"
#include "common/progress.hpp"
#include "map_stats.hpp"
#include "robin-hood-hashing/robin_hood.h"
// if we ever want to do the union-find chaining in parallel
//#include "common/dset64-gccAtomic.hpp"
// this is for single-threaded use, but is more portable
#include "common/dset64.hpp"
//#include "assert.hpp"
#include "gsl/gsl_randist.h"
#include "common/atomic_queue/atomic_queue.h"

namespace skch
{
  /**
   * @class     skch::Map
   * @brief     L1 and L2 mapping stages
   */
  class Map
  {
    public:

      //Type for Stage L1's predicted candidate location
      struct L1_candidateLocus_t
      {
        seqno_t seqId;                    //sequence id where read is mapped

        /* read could be mapped with its begin location
         * from [rangeStartPos, rangeEndPos]
         */
        offset_t rangeStartPos;
        offset_t rangeEndPos;
        int intersectionSize;
      };

      static constexpr auto L1_locus_intersection_cmp = [](L1_candidateLocus_t& a, L1_candidateLocus_t& b)
      {
        return a.intersectionSize < b.intersectionSize;
      };

      //Type for Stage L2's predicted mapping coordinate within each L1 candidate
      struct L2_mapLocus_t
      {
        seqno_t seqId;                    //sequence id where read is mapped
        offset_t meanOptimalPos;          //Among multiple consecutive optimal positions, save the avg.
        offset_t optimalStart;            //optimal start mapping position (begin iterator)
        offset_t optimalEnd;              //optimal end mapping position (end iterator)
        int sharedSketchSize;             //count of shared sketch elements
        strand_t strand;
      };

    private:

      //algorithm parameters
      const skch::Parameters &param;

      //reference sketch
      const skch::Sketch &refSketch;

      //Container type for saving read sketches during L1 and L2 both
      typedef Sketch::MI_Type MinVec_Type;

      typedef Sketch::MIIter_t MIIter_t;

      //Custom function for post processing the results, by default does nothing
      typedef std::function< void(const MappingResult&) > PostProcessResultsFn_t;
      PostProcessResultsFn_t processMappingResults;

      //Container to store query sequence name and length
      //used only if one-to-one filtering is ON
      std::vector<ContigInfo> qmetadata;

      //Vector for sketch cutoffs. Position [i] indicates the minimum intersection size required
      //for an L1 candidate if the best intersection size is i;
      std::vector<int> sketchCutoffs; 

      //Vector for obtaining group from refId
      //if refIdGroup[i] == refIdGroup[j], then sequence i and j have the same prefix;
      std::vector<int> refIdGroup; 

    public:

      /**
       * @brief                 constructor
       * @param[in] p           algorithm parameters
       * @param[in] refSketch   reference sketch
       * @param[in] f           optional user defined custom function to post process the reported mapping results
       */
      Map(const skch::Parameters &p, const skch::Sketch &refsketch,
          PostProcessResultsFn_t f = nullptr) :
        param(p),
        refSketch(refsketch),
        processMappingResults(f),
        sketchCutoffs(std::min<double>(p.sketchSize, skch::fixed::ss_table_max) + 1, 1),
        refIdGroup(refsketch.metadata.size())
    {
      if (p.stage1_topANI_filter) {
        this->setProbs();
      }
      if (p.skip_prefix)
      {
        this->setRefGroups();
      }
      this->mapQuery();
    }

    private:

      // Sets the groups of reference contigs based on prefix
      void setRefGroups()
      {
        int group = 0;
        int start_idx = 0;
        int idx = 0;
        while (start_idx < this->refSketch.metadata.size())
        {
          const auto currPrefix = prefix(this->refSketch.metadata[start_idx].name, param.prefix_delim);
          idx = start_idx;
          while (idx < this->refSketch.metadata.size()
              && currPrefix == prefix(this->refSketch.metadata[idx].name, param.prefix_delim))
          {
            this->refIdGroup[idx++] = group;
          }
          group++;
          start_idx = idx;
        }
      }

      // Gets the ref group of a query based on the prefix
      int getRefGroup(const std::string& seqName)
      {
        const auto queryPrefix = prefix(seqName, param.prefix_delim);
        for (int i = 0; i < this->refSketch.metadata.size(); i++)
        {
          const auto currPrefix = prefix(this->refSketch.metadata[i].name, param.prefix_delim);
          if (queryPrefix == currPrefix)
          {
            return this->refIdGroup[i];
          }
        }
        // Doesn't belong to any ref group
        return -1;
      }
      void setProbs() 
      {

        float deltaANI = param.ANIDiff;
        float min_p = 1 - param.ANIDiffConf;
        int ss = std::min<double>(param.sketchSize, skch::fixed::ss_table_max);

        // Cache hg pmf results
        std::vector<std::vector<double>> sketchProbs(
            ss + 1,
            std::vector<double>(ss + 1.0)
        );
        for (auto ci = 0; ci <= ss; ci++) 
        {
          for (double y = 0; y <= ci; y++) 
          {
            sketchProbs[ci][y] = gsl_ran_hypergeometric_pdf(y, ss, ss-ci, ci);
          }
        }
        
        // Return true iff Pr(ANI_i >= ANI_max - deltaANI) >= min_p
        const auto distDiff = [this, &sketchProbs, deltaANI, min_p, ss] (int cmax, int ci) {
          double prAboveCutoff = 0;
          for (double ymax = 0; ymax <= cmax; ymax++) {
            // Pr (Ymax = ymax)
            double pymax = sketchProbs[cmax][ymax];

            // yi_cutoff is minimum jaccard numerator required to be within deltaANI of ymax
            double yi_cutoff = deltaANI == 0 ? ymax : (std::floor(skch::Stat::md2j(
                skch::Stat::j2md(ymax / ss, param.kmerSize) + deltaANI, 
                param.kmerSize
            ) * ss));

            // Pr Y_i < yi_cutoff
            //std::cerr << "CMF " << yi_cutoff - 1 << " " << ss << " " << ss-ci << " " << ci << std::endl;
            double pi_acc = (yi_cutoff - 1) >= 0 ? gsl_cdf_hypergeometric_P(yi_cutoff-1, ss, ss-ci, ci) : 0;

            // Pr Y_i >= yi_cutoff
            pi_acc = 1-pi_acc;

            // Pr that mash score from cj leads to an ANI at least deltaJ less than the ANI from cmax
            prAboveCutoff += pymax * pi_acc;
            if (prAboveCutoff > min_p)
            {
              return true;
            }
          }
          return prAboveCutoff > min_p; 
        };

        // Helper vector for binary search
        std::vector<int> ss_range(ss+1);
        std::iota (ss_range.begin(), ss_range.end(), 0);

        for (auto cmax = 1; cmax <= ss; cmax++) 
        {
          // Binary search to find the lowest acceptable ci
          int ci = std::distance(
              ss_range.begin(),
              std::upper_bound(
                ss_range.begin(),
                ss_range.begin() + ss,
                false,
                [&distDiff, cmax] (bool val, int ci) {
                  return distDiff(cmax, ci);
                }
              )
          );
          sketchCutoffs[cmax] = ci;

          // For really high min_p values and some values of cmax, there are no values of
          // ci that satisfy the cutoff, so we just set to the max
          if (sketchCutoffs[cmax] == 0) {
            sketchCutoffs[cmax] = 1;
          }
        }
        //for (auto overlap = 1; overlap <= ss; overlap++) 
        //{
          //DEBUG_ASSERT(sketchCutoffs[overlap] <= overlap);
        //}
      }

      /**
       * @brief   parse over sequences in query file and map each on the reference
       */
      void mapQuery() {
          // Initialize variables
          std::atomic<seqno_t> totalReadsPickedForMapping(0);
          std::atomic<seqno_t> totalReadsMapped(0);
          std::atomic<seqno_t> seqCounter(0);

          std::ofstream outstrm(param.outFileName);
          MappingResultsVector_t allReadMappings;  // Aggregate mapping results for the complete run

          // Allowed set of queries
          std::unordered_set<std::string> allowed_query_names;
          
          // Mutex for allReadMappings
          std::mutex allReadMappings_mutex;
          if (!param.query_list.empty()) {
              std::ifstream filter_list(param.query_list);
              std::string name;
              while (getline(filter_list, name)) {
                  allowed_query_names.insert(name);
              }
          }

          // Count the total number of sequences and sequence length
          uint64_t total_seqs = 0;
          uint64_t total_seq_length = 0;
          for (const auto& fileName : param.querySequences) {
              // Check if there is a .fai file
              std::string fai_name = fileName + ".fai";
              if (fs::exists(fai_name)) {
                  std::string line;
                  std::ifstream in(fai_name.c_str());
                  while (std::getline(in, line)) {
                      auto line_split = CommonFunc::split(line, '\t');
                      auto seq_name = line_split[0];
                      bool prefix_skip = true;
                      for (const auto& prefix : param.query_prefix) {
                          if (seq_name.substr(0, prefix.size()) == prefix) {
                              prefix_skip = false;
                              break;
                          }
                      }
                      if (!allowed_query_names.empty() && allowed_query_names.find(seq_name) != allowed_query_names.end()
                          || !param.query_prefix.empty() && !prefix_skip) {
                          total_seqs++;
                          total_seq_length += std::stoul(line_split[1]);
                      }
                  }
              } else {
                  // If .fai file doesn't exist, warn and use the for_each_seq_in_file_filtered function
                  std::cerr << "[mashmap::skch::Map::mapQuery] WARNING, no .fai index found for "
                            << fileName << ", reading the file to filter query sequences (slow)" << std::endl;
                  seqiter::for_each_seq_in_file_filtered(
                      fileName,
                      param.query_prefix,
                      allowed_query_names,
                      [&](const std::string& seq_name, const std::string& seq) {
                          ++total_seqs;
                          total_seq_length += seq.size();
                      });
              }
          }

          progress_meter::ProgressMeter progress(total_seq_length, "[mashmap::skch::Map::mapQuery] mapped");

          // Initialize atomic variables
          std::atomic<bool> reader_done(false);
          std::atomic<bool> writer_done(false);
          std::atomic<uint64_t> totalReadsProcessed(0);

          // Create queues
          atomic_queue::AtomicQueue<InputSeqProgContainer*, 1024> seq_queue;
          atomic_queue::AtomicQueue<MapModuleOutput*, 1024> output_queue;

          // Mutex for qmetadata
          std::mutex qmetadata_mutex;

          // Start the reader thread
          std::thread reader_thread([this, &seq_queue, &reader_done, &totalReadsPickedForMapping, &seqCounter, &progress, &qmetadata_mutex]() {
              this->readerFunction(seq_queue, reader_done, totalReadsPickedForMapping, seqCounter, progress, qmetadata_mutex);
          });

          // Start the worker threads
          std::vector<std::thread> workers;
          std::vector<std::atomic<bool>> worker_working(param.threads);
          for (size_t i = 0; i < param.threads; ++i) {
              worker_working[i].store(false);
              workers.emplace_back([this, &seq_queue, &output_queue, &reader_done, &worker_working, i, &progress]() {
                  this->workerFunction(seq_queue, output_queue, reader_done, worker_working[i], progress);
              });
          }

          // Start the writer thread
          std::thread writer_thread([this, &output_queue, &outstrm, &progress, &writer_done, &totalReadsMapped, &worker_working, &qmetadata_mutex]() {
              this->writerFunction(output_queue, outstrm, progress, writer_done, totalReadsMapped, worker_working, qmetadata_mutex);
          });

          // Wait for the reader thread to finish
          reader_thread.join();

          // Wait for the worker threads to finish
          for (auto& worker : workers) {
              worker.join();
          }

          // Signal writer thread that processing is done
          writer_done.store(true);

          // Wait for the writer thread to finish
          writer_thread.join();

          progress.finish();

          // Output statistics
          std::cerr << "[mashmap::skch::Map::mapQuery] "
                    << "count of mapped reads = " << totalReadsMapped.load()
                    << ", reads qualified for mapping = " << totalReadsPickedForMapping.load()
                    << ", total input reads = " << seqCounter.load()
                    << ", total input bp = " << total_seq_length << std::endl;
      }

      void readerFunction(atomic_queue::AtomicQueue<InputSeqProgContainer*, 1024>& seq_queue, std::atomic<bool>& reader_done, std::atomic<seqno_t>& totalReadsPickedForMapping, std::atomic<seqno_t>& seqCounter, progress_meter::ProgressMeter& progress, std::mutex& qmetadata_mutex) {
          for (const auto& fileName : param.querySequences) {

#ifdef DEBUG
              std::cerr << "[mashmap::skch::Map::readerFunction] mapping reads in " << fileName << std::endl;
#endif

              seqiter::for_each_seq_in_file_filtered(
                  fileName,
                  param.query_prefix,
                  allowed_query_names,
                  [&](const std::string& seq_name,
                      const std::string& seq) {
                      offset_t len = seq.length();
                      if (param.skip_self
                          && param.target_prefix != ""
                          && seq_name.substr(0, param.target_prefix.size()) == param.target_prefix) {
                          // skip
                      } else {
                          if (param.filterMode == filter::ONETOONE) {
                              std::lock_guard<std::mutex> guard(qmetadata_mutex);
                              qmetadata.push_back(ContigInfo{seq_name, len});
                          }
                          // Is the read too short?
                          if (len < param.kmerSize) {
                              std::cerr << std::endl
                                        << "WARNING, skch::Map::readerFunction, read "
                                        << seq_name << " of " << len << "bp "
                                        << " is not long enough for mapping at segment length "
                                        << param.segLength << std::endl;
                          } else {
                              totalReadsPickedForMapping++;

                              // Create InputSeqProgContainer
                              InputSeqProgContainer* input = new InputSeqProgContainer(seq, seq_name, seqCounter.load(), progress);

                              // Push into seq_queue
                              while (!seq_queue.try_push(input)) {
                                  std::this_thread::sleep_for(std::chrono::milliseconds(10));
                              }
                          }
                          seqCounter++;
                      }
                  }); // Finish reading query input file
          }

          reader_done.store(true);
      }

      void workerFunction(atomic_queue::AtomicQueue<InputSeqProgContainer*, 1024>& seq_queue, atomic_queue::AtomicQueue<MapModuleOutput*, 1024>& output_queue, std::atomic<bool>& reader_done, std::atomic<bool>& is_working, progress_meter::ProgressMeter& progress) {
          is_working.store(true);
          while (true) {
              InputSeqProgContainer* input = nullptr;
              if (seq_queue.try_pop(input)) {
                  is_working.store(true);
                  // Process the sequence
                  MapModuleOutput* output = mapModule(input);
                  // Update progress
                  progress.increment(input->len);

                  delete input;

                  // Push output into output_queue
                  while (!output_queue.try_push(output)) {
                      std::this_thread::sleep_for(std::chrono::milliseconds(10));
                  }

              } else if (reader_done.load() && seq_queue.was_empty()) {
                  break;
              } else {
                  is_working.store(false);
                  std::this_thread::sleep_for(std::chrono::milliseconds(10));
              }
          }
          is_working.store(false);
      }

      void writerFunction(atomic_queue::AtomicQueue<MapModuleOutput*, 1024>& output_queue, std::ofstream& outstrm, progress_meter::ProgressMeter& progress, std::atomic<bool>& writer_done, std::atomic<seqno_t>& totalReadsMapped, std::vector<std::atomic<bool>>& worker_working, std::mutex& qmetadata_mutex) {
          auto all_workers_done = [&]() {
              return std::all_of(worker_working.begin(), worker_working.end(),
                                 [](const std::atomic<bool>& w) { return !w.load(); });
          };

          while (true) {
              MapModuleOutput* output = nullptr;
              if (output_queue.try_pop(output)) {
                  // Handle the output
                  if (output->readMappings.size() > 0)
                      totalReadsMapped++;

                  if (param.filterMode == filter::ONETOONE) {
                      // Save for another filtering round
                      std::lock_guard<std::mutex> guard(allReadMappings_mutex);
                      allReadMappings.insert(allReadMappings.end(), output->readMappings.begin(), output->readMappings.end());
                  } else {
                      // Report mapping
                      reportReadMappings(output->readMappings, output->qseqName, outstrm);
                  }

                  delete output;

              } else if (writer_done.load() && output_queue.was_empty() && all_workers_done()) {
                  break;
              } else {
                  std::this_thread::sleep_for(std::chrono::milliseconds(10));
              }
          }

          // After processing is done, if param.filterMode == filter::ONETOONE, process allReadMappings
          if (param.filterMode == filter::ONETOONE) {
              // How many secondary mappings to keep
              int n_mappings = param.numMappingsForSegment - 1;

              // Group sequences by query prefix, then pass to ref filter
              auto subrange_begin = allReadMappings.begin();
              auto subrange_end = allReadMappings.begin();
              MappingResultsVector_t tmpMappings;
              MappingResultsVector_t filteredMappings;

              while (subrange_end != allReadMappings.end()) {
                  if (param.skip_prefix) {
                      int currGroup = this->getRefGroup(qmetadata[subrange_begin->querySeqId].name);
                      subrange_end = std::find_if_not(subrange_begin, allReadMappings.end(), [this, currGroup](const auto& allReadMappings_candidate) {
                          return currGroup == this->getRefGroup(this->qmetadata[allReadMappings_candidate.querySeqId].name);
                      });
                  } else {
                      subrange_end = allReadMappings.end();
                  }
                  tmpMappings.insert(
                      tmpMappings.end(),
                      std::make_move_iterator(subrange_begin),
                      std::make_move_iterator(subrange_end));

                  // tmpMappings now contains mappings from one group of query sequences to all reference groups
                  // we now run filterByGroup, which filters based on the reference group.
                  filterByGroup(tmpMappings, filteredMappings, n_mappings, true);
                  tmpMappings.clear();
                  subrange_begin = subrange_end;
              }
              allReadMappings = std::move(filteredMappings);

              // Re-sort mappings by input order of query sequences
              // This order may be needed for any post analysis of output
              std::sort(
                  allReadMappings.begin(), allReadMappings.end(),
                  [](const MappingResult& a, const MappingResult& b) {
                      return std::tie(a.querySeqId, a.queryStartPos, a.refSeqId, a.refStartPos)
                          < std::tie(b.querySeqId, b.queryStartPos, b.refSeqId, b.refStartPos);
                  });

              reportReadMappings(allReadMappings, "", outstrm);
          }
      }

      /**
       * @brief               helper to main mapping function
       * @details             filters mappings with fewer than the target number of merged base mappings
       * @param[in]   input   mappings
       * @return              void
       */
      void filterWeakMappings(MappingResultsVector_t &readMappings, int64_t min_count)
      {
          readMappings.erase(
              std::remove_if(readMappings.begin(),
                             readMappings.end(),
                             [&](MappingResult &e){
                                 return e.blockLength < param.block_length //e.queryLen > e.blockLength
                                     || e.n_merged < min_count;
                             }),
              readMappings.end());
      }

      /**
       * @brief               helper to main mapping function
       * @details             filters mappings whose identity and query/ref length don't agree
       * @param[in]   input   mappings
       * @return              void
       */
      void filterFalseHighIdentity(MappingResultsVector_t &readMappings)
      {
          readMappings.erase(
              std::remove_if(readMappings.begin(),
                             readMappings.end(),
                             [&](MappingResult &e){
                                 int64_t q_l = (int64_t)e.queryEndPos - (int64_t)e.queryStartPos;
                                 int64_t r_l = (int64_t)e.refEndPos - (int64_t)e.refStartPos;
                                 uint64_t delta = std::abs(r_l - q_l);
                                 double len_id_bound = (1.0 - (double)delta/(((double)q_l+r_l)/2));
                                 return len_id_bound < std::min(0.7, std::pow(param.percentageIdentity,3));
                             }),
              readMappings.end());
      }

      /**
       * @brief               helper to main mapping function
       * @details             filters mappings whose split ids aren't to be kept
       * @param[in]   input   mappings
       * @param[in]   input
       * @return              void
       */
      void filterFailedSubMappings(MappingResultsVector_t &readMappings,
                                   const robin_hood::unordered_set<offset_t>& kept_chains)
      {
          readMappings.erase(
              std::remove_if(readMappings.begin(),
                             readMappings.end(),
                             [&](MappingResult &e){
                                 return kept_chains.count(e.splitMappingId) == 0;
                             }),
              readMappings.end());
      }

      /**
       * @brief               helper to main mapping function
       * @details             filters mappings by hash value
       * @param[in]   input   mappings
       * @param[in]   input
       * @return              void
       */
      void sparsifyMappings(MappingResultsVector_t &readMappings)
      {
          if (param.sparsity_hash_threshold < std::numeric_limits<uint64_t>::max()) {
              readMappings.erase(
                  std::remove_if(readMappings.begin(),
                                 readMappings.end(),
                                 [&](MappingResult &e){
                                     return e.hash() > param.sparsity_hash_threshold;
                                 }),
                  readMappings.end());
          }
      }

      /**
       * @brief                    helper to main filtering function
       * @details                  filters mappings by group
       * @param[in]   input        unfiltered mappings
       * @param[in]   output       filtered mappings
       * @param[in]   n_mappings   num mappings per segment
       * @param[in]   filter_ref   use Filter::ref instead of Filter::query
       * @return                   void
       */
      void filterByGroup(
          MappingResultsVector_t &unfilteredMappings,
          MappingResultsVector_t &filteredMappings,
          int n_mappings,
          bool filter_ref)
      {
        filteredMappings.reserve(unfilteredMappings.size());

        std::sort(unfilteredMappings.begin(), unfilteredMappings.end(), [](const auto& a, const auto& b) 
            { return std::tie(a.refSeqId, a.refStartPos) < std::tie(b.refSeqId, b.refStartPos); });
        auto subrange_begin = unfilteredMappings.begin();
        auto subrange_end = unfilteredMappings.begin();
        if (param.filterMode == filter::MAP || param.filterMode == filter::ONETOONE) 
        {
          std::vector<skch::MappingResult> tmpMappings;
          while (subrange_end != unfilteredMappings.end())
          {
            if (param.skip_prefix)
            {
              int currGroup = this->refIdGroup[subrange_begin->refSeqId];
              subrange_end = std::find_if_not(subrange_begin, unfilteredMappings.end(), [this, currGroup] (const auto& unfilteredMappings_candidate) {
                  return currGroup == this->refIdGroup[unfilteredMappings_candidate.refSeqId];
              });
            }
            else
            {
              subrange_end = unfilteredMappings.end();
            }
            tmpMappings.insert(
                tmpMappings.end(), 
                std::make_move_iterator(subrange_begin), 
                std::make_move_iterator(subrange_end));
            std::sort(tmpMappings.begin(), tmpMappings.end(), [](const auto& a, const auto& b) 
                { return std::tie(a.queryStartPos, a.refSeqId, a.refStartPos) < std::tie(b.queryStartPos, b.refSeqId, b.refStartPos); });
            if (filter_ref)
            {
                skch::Filter::ref::filterMappings(tmpMappings, this->refSketch, n_mappings, param.dropRand, param.overlap_threshold);
            }
            else
            {
                skch::Filter::query::filterMappings(tmpMappings, n_mappings, param.dropRand, param.overlap_threshold);
            }
            filteredMappings.insert(
                filteredMappings.end(), 
                std::make_move_iterator(tmpMappings.begin()), 
                std::make_move_iterator(tmpMappings.end()));
            tmpMappings.clear();
            subrange_begin = subrange_end;
          }
        }
        //Sort the mappings by query (then reference) position
        std::sort(
            filteredMappings.begin(), filteredMappings.end(),
            [](const MappingResult &a, const MappingResult &b) {
                return std::tie(a.queryStartPos, a.refSeqId, a.refStartPos) 
                  < std::tie(b.queryStartPos, b.refSeqId, b.refStartPos);
                //return std::tie(a.refSeqId, a.refStartPos, a.queryStartPos)
                    //< std::tie(b.refSeqId, b.refStartPos, b.queryStartPos);
            });
      }


      /**
       * @brief               main mapping function given an input read
       * @details             this function is run in parallel by multiple threads
       * @param[in]   input   input read details
       * @return              output object containing the mappings
       */
      MapModuleOutput* mapModule (InputSeqProgContainer* input)
      {
        MapModuleOutput* output = new MapModuleOutput();

        //save query sequence name and length
        output->qseqName = input->seqName;
        output->qseqLen = input->len;
        bool split_mapping = true;
        std::vector<IntervalPoint> intervalPoints;
        // Reserve the "expected" number of interval points
        intervalPoints.reserve(
            2 * param.sketchSize * refSketch.minmerIndex.size() / refSketch.minmerPosLookupIndex.size());
        std::vector<L1_candidateLocus_t> l1Mappings;
        MappingResultsVector_t l2Mappings;
        MappingResultsVector_t unfilteredMappings;
        int refGroup = this->getRefGroup(input->seqName);

        if (!param.split || input->len <= param.segLength)
        {
            QueryMetaData <MinVec_Type> Q;
            Q.seq = &(input->seq)[0u];
            Q.len = input->len;
            Q.fullLen = input->len;
            Q.seqCounter = input->seqCounter;
            Q.seqName = input->seqName;
            Q.refGroup = refGroup;

            // Map this sequence
            mapSingleQueryFrag(Q, intervalPoints, l1Mappings, l2Mappings);
            unfilteredMappings.insert(unfilteredMappings.end(), l2Mappings.begin(), l2Mappings.end());
        
            // Apply non-merged filtering
            filterNonMergedMappings(unfilteredMappings, param);

            split_mapping = false;
            input->progress.increment(input->len);
        }
        else // Split read mapping
        {
            int noOverlapFragmentCount = input->len / param.segLength;

            // Map individual non-overlapping fragments in the read
            for (int i = 0; i < noOverlapFragmentCount; i++)
            {
                QueryMetaData <MinVec_Type> Q;
                Q.seq = &(input->seq)[0u] + i * param.segLength;
                Q.len = param.segLength;
                Q.fullLen = input->len;
                Q.seqCounter = input->seqCounter;
                Q.seqName = input->seqName;
                Q.refGroup = refGroup;

                intervalPoints.clear();
                l1Mappings.clear();
                l2Mappings.clear();

                mapSingleQueryFrag(Q, intervalPoints, l1Mappings, l2Mappings);

                std::for_each(l2Mappings.begin(), l2Mappings.end(), [&](MappingResult &e){
                    e.queryLen = input->len;
                    e.queryStartPos = i * param.segLength;
                    e.queryEndPos = i * param.segLength + Q.len;
                });

                unfilteredMappings.insert(unfilteredMappings.end(), l2Mappings.begin(), l2Mappings.end());
                input->progress.increment(param.segLength);
            }

            // Map last overlapping fragment to cover the whole read
            if (noOverlapFragmentCount >= 1 && input->len % param.segLength != 0)
            {
                QueryMetaData <MinVec_Type> Q;
                Q.seq = &(input->seq)[0u] + input->len - param.segLength;
                Q.len = param.segLength;
                Q.seqCounter = input->seqCounter;
                Q.seqName = input->seqName;
                Q.refGroup = refGroup;

                intervalPoints.clear();
                l1Mappings.clear();
                l2Mappings.clear();

                mapSingleQueryFrag(Q, intervalPoints, l1Mappings, l2Mappings);

                std::for_each(l2Mappings.begin(), l2Mappings.end(), [&](MappingResult &e){
                    e.queryLen = input->len;
                    e.queryStartPos = input->len - param.segLength;
                    e.queryEndPos = input->len;
                });

                unfilteredMappings.insert(unfilteredMappings.end(), l2Mappings.begin(), l2Mappings.end());
                input->progress.increment(input->len % param.segLength);
            }

            if (param.mergeMappings) 
            {
                // the maximally merged mappings are top-level chains
                // while the unfiltered mappings now contain splits at max_mapping_length
                auto maximallyMergedMappings =
                    mergeMappingsInRange(unfilteredMappings, param.chain_gap);
                // we filter on the top level chains
                filterMaximallyMerged(maximallyMergedMappings, param);
                // collect splitMappingIds in the maximally merged mappings
                robin_hood::unordered_set<offset_t> kept_chains;
                for (auto &mapping : maximallyMergedMappings) {
                    kept_chains.insert(mapping.splitMappingId);
                }
                // and use them to filter mappings to discard
                unfilteredMappings.erase(
                    std::remove_if(unfilteredMappings.begin(), unfilteredMappings.end(),
                                   [&kept_chains](const MappingResult &mapping) {
                                       return !kept_chains.count(mapping.splitMappingId);
                                   }),
                    unfilteredMappings.end()
                    );
            }
            else 
            {
                filterNonMergedMappings(unfilteredMappings, param);
            }
        }

        // Common post-processing for both merged and non-merged mappings
        mappingBoundarySanityCheck(input, unfilteredMappings);
    
        if (param.filterLengthMismatches)
        {
            filterFalseHighIdentity(unfilteredMappings);
        }
    
        sparsifyMappings(unfilteredMappings);

        output->readMappings = std::move(unfilteredMappings);

        return output;
      }

      /**
       * @brief                       routine to handle mapModule's output of mappings
       * @param[in] output            mapping output object
       * @param[in] allReadMappings   vector to store mappings of all reads (optional use depending on filter)
       * @param[in] totalReadsMapped  counter to track count of reads mapped
       * @param[in] outstrm           outstream stream object
       */
      template <typename Vec>
      void mapModuleHandleOutput(MapModuleOutput* output,
                                 Vec &allReadMappings,
                                 seqno_t &totalReadsMapped,
                                 std::ofstream &outstrm,
                                 progress_meter::ProgressMeter& progress)
        {
          if(output->readMappings.size() > 0)
            totalReadsMapped++;

          if (param.filterMode == filter::ONETOONE)
          {
            //Save for another filtering round
            allReadMappings.insert(allReadMappings.end(), output->readMappings.begin(), output->readMappings.end());
          }
          else
          {
            //Report mapping
            reportReadMappings(output->readMappings, output->qseqName, outstrm);
          }

          //progress.increment(output->qseqLen/2 + (output->qseqLen % 2 != 0));

          delete output;
        }

      /**
       * @brief                       Filter non-merged mappings
       * @param[in/out] readMappings  Mappings computed by Mashmap
       * @param[in]     param         Algorithm parameters
       */
      void filterNonMergedMappings(MappingResultsVector_t &readMappings, const Parameters& param)
      {
          if (param.filterMode == filter::MAP || param.filterMode == filter::ONETOONE) {
              MappingResultsVector_t filteredMappings;
              filterByGroup(readMappings, filteredMappings, param.numMappingsForSegment - 1, false);
              readMappings = std::move(filteredMappings);
          }
      }

      /**
       * @brief                   map the parsed query sequence (L1 and L2 mapping)
       * @param[in]   Q           metadata about query sequence
       * @param[in]   outstrm     outstream stream where mappings will be reported
       * @param[out]  l2Mappings  Mapping results in the L2 stage
       */
      template<typename Q_Info, typename IPVec, typename L1Vec, typename VecOut>
        void mapSingleQueryFrag(Q_Info &Q, IPVec& intervalPoints, L1Vec& l1Mappings, VecOut &l2Mappings)
        {
#ifdef ENABLE_TIME_PROFILE_L1_L2
          auto t0 = skch::Time::now();
#endif
          //L1 Mapping
          doL1Mapping(Q, intervalPoints, l1Mappings);
          if (l1Mappings.size() == 0) {
            return;
          }

#ifdef ENABLE_TIME_PROFILE_L1_L2
          std::chrono::duration<double> timeSpentL1 = skch::Time::now() - t0;
          auto t1 = skch::Time::now();
#endif

          auto l1_begin = l1Mappings.begin();
          auto l1_end = l1Mappings.begin();
          while (l1_end != l1Mappings.end())
          {
            if (param.skip_prefix)
            {
              int currGroup = this->refIdGroup[l1_begin->seqId];
              l1_end = std::find_if_not(l1_begin, l1Mappings.end(), [this, currGroup] (const auto& candidate) {
                  return currGroup == this->refIdGroup[candidate.seqId];
              });
            }
            else
            {
              l1_end = l1Mappings.end();
            }

            //Sort L1 windows based on intersection size if using hg filter
            if (param.stage1_topANI_filter)
            {
              std::make_heap(l1_begin, l1_end, L1_locus_intersection_cmp);
            }
            doL2Mapping(Q, l1_begin, l1_end, l2Mappings);

            // Set beginning of next range
            l1_begin = l1_end;
          }

          // Sort output mappings
          std::sort(l2Mappings.begin(), l2Mappings.end(), [](const auto& a, const auto& b) 
              { return std::tie(a.refSeqId, a.refStartPos) < std::tie(b.refSeqId, b.refStartPos); });

#ifdef ENABLE_TIME_PROFILE_L1_L2
          {
            std::chrono::duration<double> timeSpentL2 = skch::Time::now() - t1;
            std::chrono::duration<double> timeSpentMappingFragment = skch::Time::now() - t0;

            std::cerr << Q.seqCounter << " " << Q.len
              << " " << timeSpentL1.count()
              << " " << timeSpentL2.count()
              << " " << timeSpentMappingFragment.count()
              << "\n";
          }
#endif
        }

      template <typename Q_Info>
        void getSeedHits(Q_Info &Q)
        {
          Q.minmerTableQuery.reserve(param.sketchSize + 1);
          CommonFunc::sketchSequence(Q.minmerTableQuery, Q.seq, Q.len, param.kmerSize, param.alphabetSize, param.sketchSize, Q.seqCounter);
          if(Q.minmerTableQuery.size() == 0) {
            Q.sketchSize = 0;
            return;
          }

#ifdef DEBUG
          int orig_len = Q.minmerTableQuery.size();
#endif
          const double max_hash_01 = (long double)(Q.minmerTableQuery.back().hash) / std::numeric_limits<hash_t>::max();
          Q.kmerComplexity = (double(Q.minmerTableQuery.size()) / max_hash_01) / ((Q.len - param.kmerSize + 1)*2);

          // TODO remove them from the original sketch instead of removing for each read
          auto new_end = std::remove_if(Q.minmerTableQuery.begin(), Q.minmerTableQuery.end(), [&](auto& mi) {
            return refSketch.isFreqSeed(mi.hash);
          });
          Q.minmerTableQuery.erase(new_end, Q.minmerTableQuery.end());

          Q.sketchSize = Q.minmerTableQuery.size();
#ifdef DEBUG
          std::cerr << "INFO, skch::Map::getSeedHits, read id " << Q.seqCounter << ", minmer count = " << Q.minmerTableQuery.size() << ", bad minmers = " << orig_len - Q.sketchSize << "\n";
#endif
        } 


      /**
       * @brief       Find candidate regions for a read using level 1 (seed-hits) mapping
       * @details     The count of hits that should occur within a region on the reference is 
       *              determined by the threshold similarity
       *              The resulting start and end target offsets on reference is (are) an 
       *              overestimate of the mapped region. Computing better bounds is left for
       *              the following L2 stage.
       * @param[in]   Q                         query sequence details 
       * @param[out]  l1Mappings                all the read mapping locations
       */
      template <typename Q_Info, typename Vec>
        void getSeedIntervalPoints(Q_Info &Q, Vec& intervalPoints)
        {

#ifdef DEBUG
          std::cerr<< "INFO, skch::Map::getSeedHits, read id " << Q.seqCounter << ", minmer count = " << Q.minmerTableQuery.size() << " " << Q.len << "\n";
#endif

          //For invalid query (example : just NNNs), we may be left with 0 sketch size
          //Ignore the query in this case
          if(Q.minmerTableQuery.size() == 0)
            return;

          // Priority queue for sorting interval points
          using IP_const_iterator = std::vector<IntervalPoint>::const_iterator;
          std::vector<boundPtr<IP_const_iterator>> pq;
          pq.reserve(Q.sketchSize);
          constexpr auto heap_cmp = [](const auto& a, const auto& b) {return b < a;};

          for(auto it = Q.minmerTableQuery.begin(); it != Q.minmerTableQuery.end(); it++)
          {
            //Check if hash value exists in the reference lookup index
            const auto seedFind = refSketch.minmerPosLookupIndex.find(it->hash);

            if(seedFind != refSketch.minmerPosLookupIndex.end())
            {
              pq.emplace_back(boundPtr<IP_const_iterator> {seedFind->second.cbegin(), seedFind->second.cend()});
            }
          }
          std::make_heap(pq.begin(), pq.end(), heap_cmp);

          while(!pq.empty())
          {
            const IP_const_iterator ip_it = pq.front().it;
            const auto& ref = this->refSketch.metadata[ip_it->seqId];
            bool skip_mapping = false;
            if (param.skip_self && Q.seqName == ref.name) skip_mapping = true;
            if (param.skip_prefix && this->refIdGroup[ip_it->seqId] == Q.refGroup) skip_mapping = true;
            if (param.lower_triangular && Q.seqCounter <= ip_it->seqId) skip_mapping = true;
    
            if (!skip_mapping) {
              intervalPoints.push_back(*ip_it);
            }
            std::pop_heap(pq.begin(), pq.end(), heap_cmp);
            pq.back().it++;
            if (pq.back().it >= pq.back().end) 
            {
              pq.pop_back();
            }
            else
            {
              std::push_heap(pq.begin(), pq.end(), heap_cmp);
            }
          }

#ifdef DEBUG
          std::cerr << "INFO, skch::Map:getSeedHits, read id " << Q.seqCounter << ", Count of seed hits in the reference = " << intervalPoints.size() / 2 << "\n";
#endif
        }


      template <typename Q_Info, typename IP_iter, typename Vec2>
        void computeL1CandidateRegions(
            Q_Info &Q, 
            IP_iter ip_begin, 
            IP_iter ip_end, 
            int minimumHits, 
            Vec2 &l1Mappings)
        {
#ifdef DEBUG
          std::cerr << "INFO, skch::Map:computeL1CandidateRegions, read id " << Q.seqCounter << std::endl;
#endif

          int overlapCount = 0;
          int strandCount = 0;
          int bestIntersectionSize = 0;
          std::vector<L1_candidateLocus_t> localOpts;

          // Keep track of all minmer windows that intersect with [i, i+windowLen]
          int windowLen = std::max<offset_t>(0, Q.len - param.segLength);
          auto trailingIt = ip_begin;
          auto leadingIt = ip_begin;

          // Group together local sketch intersection maximums that are within clusterLen of eachother
          //
          // Since setting up the L2 window [i, j] requires aggregating minmer windows over
          // [i-segLength, i), we might as well group L2 windows together which are closer than
          // segLength  
          int clusterLen = param.segLength;

          // Used to keep track of how many minmer windows for a particular hash are currently "open"
          // Only necessary when windowLen != 0.
          std::unordered_map<hash_t, int> hash_to_freq;

          if (param.stage1_topANI_filter) {
            while (leadingIt != ip_end)
            {
              // Catch the trailing iterator up to the leading iterator - windowLen
              while (
                  trailingIt != ip_end 
                  && ((trailingIt->seqId == leadingIt->seqId && trailingIt->pos <= leadingIt->pos - windowLen)
                    || trailingIt->seqId < leadingIt->seqId))
              {
                if (trailingIt->side == side::CLOSE) {
                  if (windowLen != 0)
                    hash_to_freq[trailingIt->hash]--;
                  if (windowLen == 0 || hash_to_freq[trailingIt->hash] == 0) {
                    overlapCount--;
                  }
                }
                trailingIt++;
              }
              auto currentPos = leadingIt->pos;
              while (leadingIt != ip_end && leadingIt->pos == currentPos) {
                if (leadingIt->side == side::OPEN) {
                  if (windowLen == 0 || hash_to_freq[leadingIt->hash] == 0) {
                    overlapCount++;
                  }
                  if (windowLen != 0)
                    hash_to_freq[leadingIt->hash]++;
                }
                leadingIt++;
              }

              //DEBUG_ASSERT(overlapCount >= 0, windowLen, trailingIt->seqId, trailingIt->pos, leadingIt->seqId, leadingIt->pos);
              //DEBUG_ASSERT(windowLen != 0 || overlapCount <= Q.sketchSize, windowLen, trailingIt->seqId, trailingIt->pos, leadingIt->seqId, leadingIt->pos);

              //Is this sliding window the best we have so far?
              bestIntersectionSize = std::max(bestIntersectionSize, overlapCount);
            }

            // Only go back through to find local opts if we know that there are some that are 
            // large enough
            if (bestIntersectionSize < minimumHits) 
            {
              return;
            } else 
            {
              minimumHits = std::max(
                  sketchCutoffs[
                    int(std::min(bestIntersectionSize, Q.sketchSize) 
                      / std::max<double>(1, param.sketchSize / skch::fixed::ss_table_max))
                  ],
                  minimumHits);
            }
          } 
          
          // Clear freq dict, as there will be left open CLOSE points at the end of the last seq
          // that we never got to
          hash_to_freq.clear();

          // Since there can be more than sketchSize windows that overlap w/ [i, i+windowLen]
          // cap the best intersection size 
          bestIntersectionSize = std::min(bestIntersectionSize, Q.sketchSize);

          bool in_candidate = false;
          L1_candidateLocus_t l1_out = {};
          trailingIt = ip_begin;
          leadingIt = ip_begin;

          // Keep track of 3 consecutive points so that we can track local optimums
          overlapCount = 0;
          int prevOverlap = 0;
          int prevPrevOverlap = 0;

          // Need to keep track of two positions, as the previous one will be the local optimum
          SeqCoord prevPos;
          SeqCoord currentPos{leadingIt->seqId, leadingIt->pos};


          while (leadingIt != ip_end)
          {
            prevPrevOverlap = prevOverlap;
            prevOverlap = overlapCount;

            //TODO LEADING it should only hit opens
            // We should only iterate through a new window when we come across an OPEN,
            // right now, this basically happens since every CLOSE should be an OPEN.
            // This doesn't invalidate the logic, just potentially wastes time
            while (
                trailingIt != ip_end 
                && ((trailingIt->seqId == leadingIt->seqId && trailingIt->pos <= leadingIt->pos - windowLen)
                  || trailingIt->seqId < leadingIt->seqId))
            {
              if (trailingIt->side == side::CLOSE) {
                if (windowLen != 0)
                  hash_to_freq[trailingIt->hash]--;
                if (windowLen == 0 || hash_to_freq[trailingIt->hash] == 0) {
                  overlapCount--;
                }
              }
              trailingIt++;
            }
            if (leadingIt->pos != currentPos.pos) {
              prevPos = currentPos;
              currentPos = SeqCoord{leadingIt->seqId, leadingIt->pos};
            }
            while (leadingIt != ip_end && leadingIt->pos == currentPos.pos) 
            {
              if (leadingIt->side == side::OPEN) {
                if (windowLen == 0 || hash_to_freq[leadingIt->hash] == 0) {
                  overlapCount++;
                }
                if (windowLen != 0)
                  hash_to_freq[leadingIt->hash]++;
              }
              leadingIt++;
            }
          if ( prevOverlap >= minimumHits
              //&& prevOverlap > overlapCount && prevOverlap >= prevPrevOverlap)
          ) {
            if (l1_out.seqId != prevPos.seqId && in_candidate) {
              localOpts.push_back(l1_out);
              l1_out = {};
              in_candidate = false;
            }
            if (!in_candidate) {
              l1_out.rangeStartPos = prevPos.pos - windowLen;
              l1_out.rangeEndPos = prevPos.pos - windowLen;
              l1_out.seqId = prevPos.seqId;
              l1_out.intersectionSize = prevOverlap;
              in_candidate = true;
            } else {
              if (param.stage2_full_scan) {
                l1_out.intersectionSize = std::max(l1_out.intersectionSize, prevOverlap);
                l1_out.rangeEndPos = prevPos.pos - windowLen;
              }
              else if (l1_out.intersectionSize < prevOverlap) {
                l1_out.intersectionSize = prevOverlap;
                l1_out.rangeStartPos = prevPos.pos - windowLen;
                l1_out.rangeEndPos = prevPos.pos - windowLen;
              }
            }
          } 
          else {
            if (in_candidate) {
              localOpts.push_back(l1_out);
              l1_out = {};
            }
            in_candidate = false;
          }
        }
        if (in_candidate) {
          localOpts.push_back(l1_out);
        }
        

        // Join together proximal local opts
        for (auto& l1_out : localOpts) 
        {
          if (l1Mappings.empty() 
              || l1_out.seqId != l1Mappings.back().seqId 
              || l1_out.rangeStartPos > l1Mappings.back().rangeEndPos + clusterLen) 
          {
            l1Mappings.push_back(l1_out); 
          } 
          else 
          {
            l1Mappings.back().rangeEndPos = l1_out.rangeEndPos;
            l1Mappings.back().intersectionSize = std::max(l1_out.intersectionSize, l1Mappings.back().intersectionSize);
          }
        }
      }


      /**
       * @brief       Find candidate regions for a read using level 1 (seed-hits) mapping
       * @details     The count of hits that should occur within a region on the reference is
       *              determined by the threshold similarity
       *              The resulting start and end target offsets on reference is (are) an
       *              overestimate of the mapped region. Computing better bounds is left for
       *              the following L2 stage.
       * @param[in]   Q                         query sequence details
       * @param[out]  l1Mappings                all the read mapping locations
       */
      template <typename Q_Info, typename IPVec, typename L1Vec>
        void doL1Mapping(Q_Info &Q, IPVec& intervalPoints, L1Vec& l1Mappings)
        {
          //1. Compute the minmers
          getSeedHits(Q);

          //Catch all NNNNNN case
          if (Q.sketchSize == 0 || Q.kmerComplexity < param.kmerComplexityThreshold) {
            return;
          }

          //2. Compute windows and sort
          getSeedIntervalPoints(Q, intervalPoints);

          //3. Compute L1 windows
          int minimumHits = Stat::estimateMinimumHitsRelaxed(Q.sketchSize, param.kmerSize, param.percentageIdentity, skch::fixed::confidence_interval);

          // For each "group"
          auto ip_begin = intervalPoints.begin();
          auto ip_end = intervalPoints.begin();
          while (ip_end != intervalPoints.end())
          {
            if (param.skip_prefix)
            {
              int currGroup = this->refIdGroup[ip_begin->seqId];
              ip_end = std::find_if_not(ip_begin, intervalPoints.end(), [this, currGroup] (const auto& ip) {
                  return currGroup == this->refIdGroup[ip.seqId];
              });
            }
            else
            {
              ip_end = intervalPoints.end();
            }
            computeL1CandidateRegions(Q, ip_begin, ip_end, minimumHits, l1Mappings);

            ip_begin = ip_end;
          }
        }


      // helper to get the prefix of a string
      const std::string prefix(const std::string& s, const char c) {
          //std::cerr << "prefix of " << s << " by " << c << " is " << s.substr(0, s.find_last_of(c)) << std::endl;
          return s.substr(0, s.find_last_of(c));
      }

      /**
       * @brief                                 Revise L1 candidate regions to more precise locations
       * @param[in]   Q                         query sequence information
       * @param[in]   l1Mappings                candidate regions for query sequence found at L1
       * @param[out]  l2Mappings                Mapping results in the L2 stage
       */
      template <typename Q_Info, typename L1_Iter, typename VecOut>
        void doL2Mapping(Q_Info &Q, L1_Iter l1_begin, L1_Iter l1_end, VecOut &l2Mappings)
        {
          ///2. Walk the read over the candidate regions and compute the jaccard similarity with minimum s sketches
          std::vector<L2_mapLocus_t> l2_vec;
          double bestJaccardNumerator = 0;
          auto loc_iterator = l1_begin;
          while (loc_iterator != l1_end)
          {
            L1_candidateLocus_t& candidateLocus = *loc_iterator;

            if (param.stage1_topANI_filter)
            {
              // If using HG filter, don't consider any mappings which have no chance of being 
              // within param.ANIDiff of the best mapping seen so far
              double cutoff_ani = std::max(0.0, double((1 - Stat::j2md(bestJaccardNumerator / Q.sketchSize, param.kmerSize)) - param.ANIDiff));
              double cutoff_j = Stat::md2j(1 - cutoff_ani, param.kmerSize);
              if (double(candidateLocus.intersectionSize) / Q.sketchSize < cutoff_j) 
              {
                break;
              }
            }


            l2_vec.clear();
            computeL2MappedRegions(Q, candidateLocus, l2_vec);

            for (auto& l2 : l2_vec) 
            {
              //Compute mash distance using calculated jaccard
              float mash_dist = Stat::j2md(1.0 * l2.sharedSketchSize/Q.sketchSize, param.kmerSize);

              float nucIdentity = (1 - mash_dist);
              //float nucIdentityUpperBound = getANIUBfromJaccardNum(Q.sketchSize, l2.sharedSketchSize);
              float nucIdentityUpperBound = 1 - Stat::md_lower_bound(mash_dist, Q.sketchSize, param.kmerSize, skch::fixed::confidence_interval);

              //Report the alignment if it passes our identity threshold and,
              // if we are in all-vs-all mode, it isn't a self-mapping,
              // and if we are self-mapping, the query is shorter than the target
              const auto& ref = this->refSketch.metadata[l2.seqId];
              if((param.keep_low_pct_id && nucIdentityUpperBound >= param.percentageIdentity)
                  || nucIdentity >= param.percentageIdentity)
              {
                //Track the best jaccard numerator
                bestJaccardNumerator = std::max<double>(bestJaccardNumerator, l2.sharedSketchSize);

                MappingResult res;

                //Save the output
                {
                  res.queryLen = Q.len;
                  res.refStartPos = l2.meanOptimalPos;
                  res.refEndPos = l2.meanOptimalPos + Q.len;
                  res.queryStartPos = 0;
                  res.queryEndPos = Q.len;
                  res.refSeqId = l2.seqId;
                  res.querySeqId = Q.seqCounter;
                  res.nucIdentity = nucIdentity;
                  res.nucIdentityUpperBound = nucIdentityUpperBound;
                  res.sketchSize = Q.sketchSize;
                  res.conservedSketches = l2.sharedSketchSize;
                  res.blockLength = std::max(res.refEndPos - res.refStartPos, res.queryEndPos - res.queryStartPos);
                  res.approxMatches = std::round(res.nucIdentity * res.blockLength / 100.0);
                  res.strand = l2.strand; 
                  res.kmerComplexity = Q.kmerComplexity;

                  res.selfMapFilter = ((param.skip_self || param.skip_prefix) && Q.fullLen > ref.len);

                } 
                l2Mappings.push_back(res);
              }
            }

            if (param.stage1_topANI_filter) 
            {
              std::pop_heap(l1_begin, l1_end, L1_locus_intersection_cmp); 
              l1_end--; //"Pop back" 
            }
            else 
            {
              loc_iterator++;
            }
          }
          //std::cerr << "For an segment with " << l1Mappings.size()
            //<< " L1 mappings "
            //<< " there were " << l2Mappings.size() << " L2 mappings\n";
        }

      /**
       * @brief                                 Find optimal mapping within an L1 candidate
       * @param[in]   Q                         query sequence information
       * @param[in]   candidateLocus            L1 candidate location
       * @param[out]  l2_out                    L2 mapping inside L1 candidate
       */
      template <typename Q_Info, typename Vec>
        void computeL2MappedRegions(Q_Info &Q,
            L1_candidateLocus_t &candidateLocus,
            Vec &l2_vec_out)
        {
#ifdef DEBUG
          //std::cerr << "INFO, skch::Map:computeL2MappedRegions, read id " << Q.seqName << "_" << Q.startPos << std::endl; 
#endif
           
          auto& minmerIndex = refSketch.minmerIndex;

          //candidateLocus.rangeStartPos -= param.segLength;
          //candidateLocus.rangeEndPos += param.segLength;
          
          // Get first potential mashimizer
          const MinmerInfo first_minmer = MinmerInfo {0, candidateLocus.rangeStartPos - param.segLength - 1, 0, candidateLocus.seqId, 0};

          //const MinmerInfo first_minmer = MinmerInfo {0, candidateLocus.seqId, -1, 0, 0};
          auto firstOpenIt = std::lower_bound(minmerIndex.begin(), minmerIndex.end(), first_minmer); 

          // Keeps track of the lowest end position
          std::vector<skch::MinmerInfo> slidingWindow;
          slidingWindow.reserve(Q.sketchSize);

          // Used to make a min-heap
          constexpr auto heap_cmp = [](const skch::MinmerInfo& l, const skch::MinmerInfo& r) {return l.wpos_end > r.wpos_end;};

          // windowIt keeps track of the end of window
          auto windowIt = firstOpenIt;

          // Keep track of all minmer windows that intersect with [i, i+windowLen]
          int windowLen = std::max<offset_t>(0, Q.len - param.segLength);

          // Used to keep track of how many minmer windows for a particular hash are currently "open"
          // Only necessary when windowLen != 0.
          std::unordered_map<hash_t, int> hash_to_freq;
          
          // slideMap tracks the S(A or B) and S(A) and S(B)
          SlideMapper<Q_Info> slideMap(Q);

          offset_t beginOptimalPos = 0;
          offset_t lastOptimalPos = 0;
          int bestSketchSize = 1;
          int bestIntersectionSize = 0;
          bool in_candidate = false;
          L2_mapLocus_t l2_out = {};

          // Set up the window
          while (windowIt != minmerIndex.end() && windowIt->seqId == candidateLocus.seqId && windowIt->wpos < candidateLocus.rangeStartPos) 
          {
            if (windowIt->wpos_end > candidateLocus.rangeStartPos) 
            {
              if (windowLen > 0) 
              {
                hash_to_freq[windowIt->hash]++;
              }
              if (windowLen == 0 || hash_to_freq[windowIt->hash] == 1) {
                slidingWindow.push_back(*windowIt);
                std::push_heap(slidingWindow.begin(), slidingWindow.end(), heap_cmp);
                slideMap.insert_minmer(*windowIt);
              }
            }
            windowIt++;
          }

          while (windowIt != minmerIndex.end() && windowIt->seqId == candidateLocus.seqId && windowIt->wpos <= candidateLocus.rangeEndPos + windowLen) 
          {
            int prev_strand_votes = slideMap.strand_votes;
            bool inserted = false;
            while (!slidingWindow.empty() && slidingWindow.front().wpos_end <= windowIt->wpos - windowLen) {

              // Remove minmer from end-ordered heap
              if (windowLen > 0) 
              {
                hash_to_freq[slidingWindow.front().hash]--;
              }
              if (windowLen == 0 || hash_to_freq[slidingWindow.front().hash] == 0) {
                // Remove minmer from  sorted window
                slideMap.delete_minmer(slidingWindow.front());
                std::pop_heap(slidingWindow.begin(), slidingWindow.end(), heap_cmp);
                slidingWindow.pop_back();
              }

            }
            inserted = true;
            if (windowLen > 0) 
            {
              hash_to_freq[windowIt->hash]++;
            }
            if (windowLen == 0 || hash_to_freq[windowIt->hash] == 1) {
              slideMap.insert_minmer(*windowIt);
              slidingWindow.push_back(*windowIt);
              std::push_heap(slidingWindow.begin(), slidingWindow.end(), heap_cmp);
            } else {
              windowIt++;
              continue;
            }

            bestIntersectionSize = std::max(bestIntersectionSize, slideMap.intersectionSize);

            //Is this sliding window the best we have so far?
            if (slideMap.sharedSketchElements > bestSketchSize)
            {
              // Get rid of all candidates seen so far
              l2_vec_out.clear();

              in_candidate = true;
              bestSketchSize = slideMap.sharedSketchElements;
              l2_out.sharedSketchSize = slideMap.sharedSketchElements;

              //Save the position
              l2_out.optimalStart = windowIt->wpos - windowLen;
              l2_out.optimalEnd = windowIt->wpos - windowLen;
            }
            else if(slideMap.sharedSketchElements == bestSketchSize)
            {
              if (!in_candidate) {
                l2_out.sharedSketchSize = slideMap.sharedSketchElements;

                //Save the position
                l2_out.optimalStart = windowIt->wpos - windowLen;
              }

              in_candidate = true;
              //Still save the position
              l2_out.optimalEnd = windowIt->wpos - windowLen;
            } else {
              if (in_candidate) {
                // Save and reset
                l2_out.meanOptimalPos =  (l2_out.optimalStart + l2_out.optimalEnd) / 2;
                l2_out.seqId = windowIt->seqId;
                l2_out.strand = prev_strand_votes >= 0 ? strnd::FWD : strnd::REV;
                if (l2_vec_out.empty() 
                    || l2_vec_out.back().optimalEnd + param.segLength < l2_out.optimalStart)
                {
                  l2_vec_out.push_back(l2_out);
                }
                else 
                {
                  l2_vec_out.back().optimalEnd = l2_out.optimalEnd;
                  l2_vec_out.back().meanOptimalPos = (l2_vec_out.back().optimalStart + l2_vec_out.back().optimalEnd) / 2;
                }
                l2_out = L2_mapLocus_t();
              }
              in_candidate = false;
            }
            if (inserted) {
              windowIt++;
            }
          }
          if (in_candidate) {
            // Save and reset
            l2_out.meanOptimalPos =  (l2_out.optimalStart + l2_out.optimalEnd) / 2;
            l2_out.seqId = std::prev(windowIt)->seqId;
            l2_out.strand = slideMap.strand_votes >= 0 ? strnd::FWD : strnd::REV;
            if (l2_vec_out.empty() 
                || l2_vec_out.back().optimalEnd + param.segLength < l2_out.optimalStart)
            {
              l2_vec_out.push_back(l2_out);
            }
            else 
            {
              l2_vec_out.back().optimalEnd = l2_out.optimalEnd;
              l2_vec_out.back().meanOptimalPos = (l2_vec_out.back().optimalStart + l2_vec_out.back().optimalEnd) / 2;
            }
          }
        }


      /**
       * @brief                       Merge the consecutive fragment mappings reported in each query
       * @param[in/out] readMappings  Mappings computed by Mashmap (L2 stage) for a read
       */
      template <typename VecIn>
        void expandMappings(VecIn &readMappings, int expansion)
        {
            for (auto& m : readMappings) {
                m.refStartPos -= expansion;
                m.refEndPos += expansion;
                m.queryStartPos -= expansion;
                m.queryEndPos += expansion;
            }
        }

      void processMappingFragment(std::vector<MappingResult>::iterator start, std::vector<MappingResult>::iterator end) {
          auto& fragment = *start; // We'll update the first mapping in the fragment

          //std::cerr << "Processing fragment" << std::endl;
          // Compute fragment information
          for (auto it = start; it != end; ++it) {
              //std::cerr << "Merging " << it->queryStartPos << " " << it->queryEndPos << " " 
              //          << it->refStartPos << " " << it->refEndPos << " " << it->nucIdentity
              //          << " " << (it->strand == strnd::FWD ? "+" : "-") << std::endl;
        
              fragment.queryStartPos = std::min(fragment.queryStartPos, it->queryStartPos);
              fragment.refStartPos = std::min(fragment.refStartPos, it->refStartPos);
              fragment.queryEndPos = std::max(fragment.queryEndPos, it->queryEndPos);
              fragment.refEndPos = std::max(fragment.refEndPos, it->refEndPos);
          }

          fragment.blockLength = std::max(fragment.refEndPos - fragment.refStartPos, fragment.queryEndPos - fragment.queryStartPos);
          //fragment.blockLength = fragment.queryEndPos - fragment.queryStartPos;
                                          
          fragment.approxMatches = std::round(fragment.nucIdentity * fragment.blockLength / 100.0);

          fragment.n_merged = std::distance(start, end);

          // Calculate mean nucleotide identity
          fragment.nucIdentity = std::accumulate(start, end, 0.0,
                                                 [](double sum, const MappingResult& e) { return sum + e.nucIdentity; }
              ) / fragment.n_merged;

          // Calculate mean kmer complexity
          fragment.kmerComplexity = std::accumulate(start, end, 0.0,
                                                    [](double sum, const MappingResult& e) { return sum + e.kmerComplexity; }
              ) / fragment.n_merged;

          // Mark other mappings in this fragment for discard
          std::for_each(std::next(start), end, [](MappingResult& e) { e.discard = 1; });
      }

      void adjustConsecutiveMappings(std::vector<MappingResult>::iterator begin_maping,
                                     std::vector<MappingResult>::iterator end_mapping,
                                     const int threshold) {

          if (std::distance(begin_maping, end_mapping) < 2) return;

          //for (size_t i = 1; i < mappings.size(); ++i) {
          for (auto it = std::next(begin_maping); it != end_mapping; ++it) {
              auto& prev = *std::prev(it);
              auto& curr = *it;

              // Check if mappings are on the same reference sequence
              if (prev.refSeqId != curr.refSeqId
                  || prev.strand != curr.strand) continue;

              // Calculate gaps
              int query_gap = curr.queryStartPos - prev.queryEndPos;
              int ref_gap = curr.refStartPos - prev.refEndPos;

              // Check if both gaps are >0 and within the threshold
              if (query_gap > 0 && ref_gap > 0 && query_gap <= threshold && ref_gap <= threshold) {
                  // Calculate midpoints
                  int query_mid = (prev.queryEndPos + curr.queryStartPos) / 2;
                  int ref_mid = (prev.refEndPos + curr.refStartPos) / 2;

                  // Adjust the mappings
                  prev.queryEndPos = query_mid;
                  prev.refEndPos = ref_mid;
                  curr.queryStartPos = query_mid;
                  curr.refStartPos = ref_mid;

                  // Update block lengths
                  prev.blockLength = std::max(prev.refEndPos - prev.refStartPos, 
                                              prev.queryEndPos - prev.queryStartPos);
                  curr.blockLength = std::max(curr.refEndPos - curr.refStartPos, 
                                              curr.queryEndPos - curr.queryStartPos);

                  // Update approximate matches
                  prev.approxMatches = std::round(prev.nucIdentity * prev.blockLength / 100.0);
                  curr.approxMatches = std::round(curr.nucIdentity * curr.blockLength / 100.0);
              }
          }
      }

      double axis_weighted_euclidean_distance(int64_t dx, int64_t dy, double w = 0.5) {
          double euclidean = std::sqrt(dx*dx + dy*dy);
          double axis_factor = 1.0 - (2.0 * std::min(std::abs(dx), std::abs(dy))) / (std::abs(dx) + std::abs(dy));
          return euclidean * (1.0 + w * axis_factor);
      }

      /**
       * @brief                       Filter maximally merged mappings
       * @param[in]   readMappings    Merged mappings computed by Mashmap
       * @param[in]   param           Algorithm parameters
       * @return                      Filtered mappings
       */
      void filterMaximallyMerged(MappingResultsVector_t& readMappings, const Parameters& param)
      {
          // Filter weak mappings
          filterWeakMappings(readMappings, std::floor(param.block_length / param.segLength));

          // Apply group filtering if necessary
          if (param.filterMode == filter::MAP || param.filterMode == filter::ONETOONE) {
              MappingResultsVector_t groupFilteredMappings;
              filterByGroup(readMappings, groupFilteredMappings, param.numMappingsForSegment - 1, false);
              readMappings = std::move(groupFilteredMappings);
          }

      }

      /**
       * @brief                       Merge fragment mappings by convolution of a 2D range over the alignment matrix
       * @param[in/out] readMappings  Mappings computed by Mashmap (L2 stage) for a read
       * @param[in]     max_dist      Distance to look in target and query
       */
      template <typename VecIn>
      VecIn mergeMappingsInRange(VecIn &readMappings,
                                int max_dist) {
          assert(param.split == true);

          if(readMappings.size() < 2) return readMappings;

          //Sort the mappings by query position, then reference sequence id, then reference position
          std::sort(
              readMappings.begin(), readMappings.end(),
              [](const MappingResult &a, const MappingResult &b) {
                  return std::tie(a.queryStartPos, a.refSeqId, a.refStartPos)
                      < std::tie(b.queryStartPos, b.refSeqId, b.refStartPos);
              });

          //First assign a unique id to each split mapping in the sorted order
          for (auto it = readMappings.begin(); it != readMappings.end(); it++) {
              it->splitMappingId = std::distance(readMappings.begin(), it);
              it->discard = 0;
              it->chainPairScore = std::numeric_limits<double>::max();
              it->chainPairId = std::numeric_limits<int64_t>::min();
          }

          // set up our union find data structure to track merges
          std::vector<dsets::DisjointSets::Aint> ufv(readMappings.size());
          // this initializes everything
          auto disjoint_sets = dsets::DisjointSets(ufv.data(), ufv.size());

          //Start the procedure to identify the chains
          for (auto it = readMappings.begin(); it != readMappings.end(); it++) {
              double best_score = std::numeric_limits<double>::max();
              auto best_it2 = readMappings.end();
              // we we merge only with the best-scored previous mapping in query space
              if (it->chainPairScore != std::numeric_limits<double>::max()) {
                  disjoint_sets.unite(it->splitMappingId, it->chainPairId);
              }
              for (auto it2 = std::next(it); it2 != readMappings.end(); it2++) {
                  // If this mapping is for a different reference sequence, ignore
                  if (it2->refSeqId != it->refSeqId) {
                      continue;
                  }
                  //If this mapping is for exactly the same segment, ignore
                  if (it2->queryStartPos == it->queryStartPos) {
                      continue;
                  }
                  //If this mapping is too far from current mapping being evaluated in the query, stop finding a merge
                  if (it2->queryStartPos > it->queryEndPos + max_dist) {
                      break;
                  }
                  //If the next mapping is within range, we can potentially merge
                  if (it2->strand == it->strand) {
                      // Always calculate query distance the same way, as query always moves forward
                      int64_t query_dist = it2->queryStartPos - it->queryEndPos;

                      // Reference distance calculation depends on strand
                      int64_t ref_dist;
                      if (it->strand == strnd::FWD) {
                          ref_dist = it2->refStartPos - it->refEndPos;
                      } else {
                          // For reverse complement, we need to invert the order
                          ref_dist = it->refStartPos - it2->refEndPos;
                      }

                      // Check if the distance is within acceptable range
                      if (query_dist >= 0 && ref_dist >= -param.segLength/5 && ref_dist <= max_dist) {
                          double dist = std::sqrt(std::pow(query_dist, 2) + std::pow(ref_dist, 2));
                          if (dist < max_dist && best_score > dist && it2->chainPairScore > dist) {
                              best_it2 = it2;
                              best_score = dist;
                          }
                      }
                  }
              }
              if (best_it2 != readMappings.end()) {
                  best_it2->chainPairScore = best_score;
                  best_it2->chainPairId = it->splitMappingId;
              }
          }

          // Assign the merged mapping ids
          for (auto it = readMappings.begin(); it != readMappings.end(); it++) {
              it->splitMappingId = disjoint_sets.find(it->splitMappingId);
          }

          //Sort the mappings by post-merge split mapping id, then by query position, then by target position
          std::sort(
              readMappings.begin(),
              readMappings.end(),
              [](const MappingResult &a, const MappingResult &b) {
                  return std::tie(a.splitMappingId, a.queryStartPos, a.refSeqId, a.refStartPos)
                      < std::tie(b.splitMappingId, b.queryStartPos, b.refSeqId, b.refStartPos);
              });

          // Create maximallyMergedMappings
          MappingResultsVector_t maximallyMergedMappings;
          for(auto it = readMappings.begin(); it != readMappings.end();) {
              auto it_end = std::find_if(it, readMappings.end(), [&](const MappingResult &e){return e.splitMappingId != it->splitMappingId;} );
              MappingResult mergedMapping = *it;  // Copy all fields from the first mapping in the chain
              mergedMapping.queryStartPos = it->queryStartPos;
              mergedMapping.queryEndPos = std::prev(it_end)->queryEndPos;
              mergedMapping.refStartPos = it->refStartPos;
              mergedMapping.refEndPos = std::prev(it_end)->refEndPos;
              mergedMapping.blockLength = std::max(mergedMapping.refEndPos - mergedMapping.refStartPos,
                                                   mergedMapping.queryEndPos - mergedMapping.queryStartPos);
              mergedMapping.n_merged = std::distance(it, it_end);
              
              // Recalculate average values for the merged mapping
              double totalNucIdentity = 0.0;
              double totalKmerComplexity = 0.0;
              int totalConservedSketches = 0;
              int totalSketchSize = 0;
              for (auto subIt = it; subIt != it_end; ++subIt) {
                  totalNucIdentity += subIt->nucIdentity;
                  totalKmerComplexity += subIt->kmerComplexity;
                  totalConservedSketches += subIt->conservedSketches;
                  totalSketchSize += subIt->sketchSize;
              }
              mergedMapping.nucIdentity = totalNucIdentity / mergedMapping.n_merged;
              mergedMapping.kmerComplexity = totalKmerComplexity / mergedMapping.n_merged;
              mergedMapping.conservedSketches = totalConservedSketches;
              mergedMapping.sketchSize = totalSketchSize;
              
              // Calculate blockNucIdentity
              mergedMapping.blockNucIdentity = mergedMapping.nucIdentity;
              
              // Ensure other fields are properly set
              mergedMapping.approxMatches = std::round(mergedMapping.nucIdentity * mergedMapping.blockLength / 100.0);
              mergedMapping.discard = 0;
              mergedMapping.overlapped = false;
              mergedMapping.chainPairScore = std::numeric_limits<double>::max();
              mergedMapping.chainPairId = std::numeric_limits<int64_t>::min();

              maximallyMergedMappings.push_back(mergedMapping);
              it = it_end;
          }

          for(auto it = readMappings.begin(); it != readMappings.end();) {
              //Bucket by each chain
              auto it_end = std::find_if(it, readMappings.end(), [&](const MappingResult &e){return e.splitMappingId != it->splitMappingId;} );

              // Process the chain into chunks defined by max_mapping_length
              processChainWithSplits(it, it_end);

              // Move the iterator to the end of the processed chain
              it = it_end;
          }

          // After processing all chains, remove discarded mappings
          readMappings.erase(
              std::remove_if(readMappings.begin(), readMappings.end(), 
                             [](const MappingResult& e) { return e.discard == 1; }),
              readMappings.end()
          );

          return maximallyMergedMappings;
      }

      /**
       * @brief Process a chain of mappings, potentially splitting it into smaller fragments
       * @param begin Iterator to the start of the chain
       * @param end Iterator to the end of the chain
       */
      void processChainWithSplits(std::vector<MappingResult>::iterator begin, std::vector<MappingResult>::iterator end) {
          if (begin == end) return;

          std::vector<bool> is_cuttable(std::distance(begin, end), true);
          
          // Mark positions that are not cuttable (near discontinuities)
          for (auto it = std::next(begin); it != end; ++it) {
              auto prev = std::prev(it);
              if (it->queryStartPos - prev->queryEndPos > param.segLength / 5 ||
                  it->refStartPos - prev->refEndPos > param.segLength / 5) {
                  is_cuttable[std::distance(begin, prev)] = false;
                  is_cuttable[std::distance(begin, it)] = false;
              }
          }

          adjustConsecutiveMappings(begin, end, param.segLength);

          auto fragment_start = begin;
          offset_t accumulate_length = 0;

          for (auto it = begin; it != end; ++it) {
              accumulate_length += it->queryEndPos - it->queryStartPos;
              
              if (accumulate_length >= param.max_mapping_length && is_cuttable[std::distance(begin, it)]) {
                  // Process the fragment up to this point
                  processMappingFragment(fragment_start, std::next(it));
                  
                  // Start a new fragment
                  fragment_start = std::next(it);
                  accumulate_length = 0;
              }
          }

          // Process any remaining fragment
          if (fragment_start != end) {
              processMappingFragment(fragment_start, end);
          }

          // Compute and assign chain statistics
          computeChainStatistics(begin, end);
      }

      /**
       * @brief Compute and assign chain statistics to all mappings in the chain
       * @param begin Iterator to the start of the chain
       * @param end Iterator to the end of the chain
       */
      void computeChainStatistics(std::vector<MappingResult>::iterator begin, std::vector<MappingResult>::iterator end) {
          offset_t chain_start_query = std::numeric_limits<offset_t>::max();
          offset_t chain_end_query = std::numeric_limits<offset_t>::min();
          offset_t chain_start_ref = std::numeric_limits<offset_t>::max();
          offset_t chain_end_ref = std::numeric_limits<offset_t>::min();
          double accumulate_nuc_identity = 0.0;
          int n_in_full_chain = std::distance(begin, end);

          for (auto it = begin; it != end; ++it) {
              chain_start_query = std::min(chain_start_query, it->queryStartPos);
              chain_end_query = std::max(chain_end_query, it->queryEndPos);
              chain_start_ref = std::min(chain_start_ref, it->refStartPos);
              chain_end_ref = std::max(chain_end_ref, it->refEndPos);
              accumulate_nuc_identity += it->nucIdentity;
          }

          auto chain_nuc_identity = accumulate_nuc_identity / n_in_full_chain;
          auto block_length = std::max(chain_end_query - chain_start_query, chain_end_ref - chain_start_ref);

          for (auto it = begin; it != end; ++it) {
              it->n_merged = n_in_full_chain;
              it->blockLength = block_length;
              it->blockNucIdentity = chain_nuc_identity;
          }
      }

     /**
       * @brief                       This routine is to make sure that all mapping boundaries
       *                              on query and reference are not outside total
       *                              length of sequeunces involved
       * @param[in]     input         input read details
       * @param[in/out] readMappings  Mappings computed by Mashmap (L2 stage) for a read
       */
      template <typename VecIn>
        void mappingBoundarySanityCheck(InputSeqProgContainer* input, VecIn &readMappings)
        {
          for(auto &e : readMappings)
          {
            //reference start pos
            {
              if(e.refStartPos < 0)
                e.refStartPos = 0;
              if(e.refStartPos >= this->refSketch.metadata[e.refSeqId].len)
                e.refStartPos = this->refSketch.metadata[e.refSeqId].len - 1;
            }

            //reference end pos
            {
              if(e.refEndPos < e.refStartPos)
                e.refEndPos = e.refStartPos;
              if(e.refEndPos >= this->refSketch.metadata[e.refSeqId].len)
                e.refEndPos = this->refSketch.metadata[e.refSeqId].len - 1;
            }

            //query start pos
            {
              if(e.queryStartPos < 0)
                e.queryStartPos = 0;
              if(e.queryStartPos >= input->len)
                e.queryStartPos = input->len;
            }

            //query end pos
            {
              if(e.queryEndPos < e.queryStartPos)
                e.queryEndPos = e.queryStartPos;
              if(e.queryEndPos >= input->len)
                e.queryEndPos = input->len;
            }
          }
        }

      /**
       * @brief                         Report the final read mappings to output stream
       * @param[in]   readMappings      mapping results for single or multiple reads
       * @param[in]   queryName         input required if reporting one read at a time
       * @param[in]   outstrm           file output stream object
       */
      void reportReadMappings(MappingResultsVector_t &readMappings, const std::string &queryName,
          std::ofstream &outstrm)
      {
        //Print the results
        for(auto &e : readMappings)
        {
          assert(e.refSeqId < this->refSketch.metadata.size());

          float fakeMapQ = e.nucIdentity == 1 ? 255 : std::round(-10.0 * std::log10(1-(e.nucIdentity)));
          std::string sep = param.legacy_output ? " " : "\t";

          outstrm  << (param.filterMode == filter::ONETOONE ? qmetadata[e.querySeqId].name : queryName)
                   << sep << e.queryLen
                   << sep << e.queryStartPos
                   << sep << e.queryEndPos - (param.legacy_output ? 1 : 0)
                   << sep << (e.strand == strnd::FWD ? "+" : "-")
                   << sep << this->refSketch.metadata[e.refSeqId].name
                   << sep << this->refSketch.metadata[e.refSeqId].len
                   << sep << e.refStartPos
                   << sep << e.refEndPos - (param.legacy_output ? 1 : 0);

          if (!param.legacy_output) 
          {
            outstrm  << sep << e.conservedSketches
                     << sep << e.blockLength
                     << sep << fakeMapQ
                     << sep << "id:f:" << e.nucIdentity
                     << sep << "kc:f:" << e.kmerComplexity;
            if (!param.mergeMappings) 
            {
              outstrm << sep << "jc:f:" << float(e.conservedSketches) / e.sketchSize;
            } else {
              outstrm << sep << "chain:i:" << e.splitMappingId;
            }
          } else
          {
            outstrm << sep << e.nucIdentity * 100.0;
          }

#ifdef DEBUG
          outstrm << std::endl;
#else
          outstrm << "\n";
#endif

          //User defined processing of the results
          if(processMappingResults != nullptr)
            processMappingResults(e);
        }
      }

    public:

      /**
       * @brief     An optional utility function to save the
       *            reported results by the L2 stage into a vector
       */
      static void insertL2ResultsToVec(MappingResultsVector_t &v, const MappingResult &reportedL2Result)
      {
        v.push_back(reportedL2Result);
      }

  };

}

#endif
