/**
 * @file    winSketch.hpp
 * @brief   routines to index the reference 
 * @author  Chirag Jain <cjain7@gatech.edu>
 */

#ifndef WIN_SKETCH_HPP 
#define WIN_SKETCH_HPP

#include <algorithm>
#include <cassert>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <filesystem>
namespace fs = std::filesystem;

//#include <zlib.h>

//Own includes
#include "map/include/base_types.hpp"
#include "map/include/map_parameters.hpp"
#include "map/include/commonFunc.hpp"
#include "map/include/ThreadPool.hpp"

//External includes
#include "common/murmur3.h"
#include "common/prettyprint.hpp"
#include "csv.h"

//#include "common/sparsehash/dense_hash_map"
//#include "common/parallel-hashmap/parallel_hashmap/phmap.h"
//#include <abseil-cpp/absl/container/flat_hash_map.h>
//#include <common/sparse-map/include/tsl/sparse_map.h>
//#include <common/robin-hood-hashing/robin_hood.h>
#include "common/ankerl/unordered_dense.hpp"

#include "common/seqiter.hpp"
#include "common/atomic_queue/atomic_queue.h"
#include "sequenceIds.hpp"
#include "common/atomic_queue/atomic_queue.h"
#include "common/progress.hpp"
#include <thread>
#include <atomic>

//#include "assert.hpp"

namespace skch
{
  /**
   * @class     skch::Sketch
   * @brief     sketches and indexes the reference (subject sequence)
   * @details  
   *            1.  Minmers are computed in streaming fashion
   *                Computing minmers is using double ended queue which gives
   *                O(reference size) complexity
   *                Algorithm described here:
   *                https://people.cs.uct.ac.za/~ksmith/articles/sliding_window_minimum.html
   *
   *            2.  Index hashes into appropriate format to enable fast search at L1 mapping stage
   */
  class Sketch
    {
      //private members
    
      //algorithm parameters
      skch::Parameters param;

      //Make the default constructor protected, non-accessible
      protected:
      Sketch(SequenceIdManager& idMgr) : idManager(idMgr) {}

      public:

      //Flag to indicate if the Sketch is fully initialized
      bool isInitialized = false;

      using MI_Type = std::vector< MinmerInfo >;
      using MIIter_t = MI_Type::const_iterator;
      using HF_Map_t = ankerl::unordered_dense::map<hash_t, uint64_t>;

      public:
        uint64_t total_seq_length = 0;

      //Index for fast seed lookup (unordered_map)
      /*
       * [minmer #1] -> [pos1, pos2, pos3 ...]
       * [minmer #2] -> [pos1, pos2...]
       * ...
       */
      //using MI_Map_t = google::dense_hash_map< MinmerMapKeyType, MinmerMapValueType >;
      //using MI_Map_t = phmap::flat_hash_map< MinmerMapKeyType, MinmerMapValueType >;
      //using MI_Map_t = absl::flat_hash_map< MinmerMapKeyType, MinmerMapValueType >;
      //using MI_Map_t = tsl::sparse_map< MinmerMapKeyType, MinmerMapValueType >;
      using MI_Map_t = ankerl::unordered_dense::map< MinmerMapKeyType, MinmerMapValueType >;
      MI_Map_t minmerPosLookupIndex;
      MI_Type minmerIndex;

      // Atomic queues for input and output
      using input_queue_t = atomic_queue::AtomicQueue<InputSeqContainer*, 1024>;
      using output_queue_t = atomic_queue::AtomicQueue<std::pair<uint64_t, MI_Type*>*, 1024>;
      input_queue_t input_queue;
      output_queue_t output_queue;

      double hgNumerator;

      private:

      /**
       * Keep list of minmers, sequence# , their position within seq , here while parsing sequence 
       * Note : position is local within each contig
       * Hashes saved here are non-unique, ordered as they appear in the reference
       */

      //Frequency histogram of minmers
      //[... ,x -> y, ...] implies y number of minmers occur x times
      std::map<uint64_t, uint64_t> minmerFreqHistogram;

      // Instance of the SequenceIdManager
      SequenceIdManager& idManager;

      public:

      /**
       * @brief   constructor
       *          also builds, indexes the minmer table
       */
      Sketch(skch::Parameters p,
             SequenceIdManager& idMgr,
             const std::vector<std::string>& targets = {},
             std::ifstream* indexStream = nullptr)
        : param(std::move(p)),
          idManager(idMgr)
      {
        if (indexStream) {
          readIndex(*indexStream, targets);
        } else {
          initialize(targets);
        }
      }

    public:
      void initialize(const std::vector<std::string>& targets = {}) {
        std::cerr << "[mashmap::skch::Sketch] Initializing Sketch..." << std::endl;
        
        this->build(true, targets);

        this->hgNumerator = param.hgNumerator;
        std::cerr << "[mashmap::skch::Sketch] Using HG numerator: " << hgNumerator << std::endl;

        std::cerr << "[mashmap::skch::Sketch] Unique minmer hashes = " << minmerPosLookupIndex.size() << std::endl;
        std::cerr << "[mashmap::skch::Sketch] Total minmer windows after pruning = " << minmerIndex.size() << std::endl;
        std::cerr << "[mashmap::skch::Sketch] Number of sequences = " << targets.size() << std::endl;
        std::cerr << "[mashmap::skch::Sketch] HG numerator: " << hgNumerator << std::endl;
        isInitialized = true;
        std::cerr << "[mashmap::skch::Sketch] Sketch initialization complete." << std::endl;
      }

      // Removed determineGlobalJaccardNumerator function

      private:
      void reader_thread(const std::vector<std::string>& targets, std::atomic<bool>& reader_done) {
          for (const auto& fileName : param.refSequences) {
              seqiter::for_each_seq_in_file(
                  fileName,
                  targets,
                  [&](const std::string& seq_name, const std::string& seq) {
                      if (seq.length() >= param.segLength) {
                          seqno_t seqId = idManager.getSequenceId(seq_name);
                          auto record = new InputSeqContainer(seq, seq_name, seqId);
                          input_queue.push(record);
                      }
                      // We don't update progress here anymore
                  });
          }
          reader_done.store(true);
      }

      void worker_thread(std::atomic<bool>& reader_done, progress_meter::ProgressMeter& progress) {
          while (true) {
              InputSeqContainer* record = nullptr;
              if (input_queue.try_pop(record)) {
                  auto minmers = new MI_Type();
                  CommonFunc::addMinmers(*minmers, &(record->seq[0]), record->len, 
                                         param.kmerSize, param.segLength, param.alphabetSize, 
                                         param.sketchSize, record->seqId);
                  auto output_pair = new std::pair<uint64_t,MI_Type*>(record->len, minmers);
                  output_queue.push(output_pair);
                  delete record;
              } else if (reader_done.load() && input_queue.was_empty()) {
                  break;
              } else {
                  std::this_thread::sleep_for(std::chrono::milliseconds(10));
              }
          }
      }

      void writer_thread(std::atomic<bool>& workers_done, progress_meter::ProgressMeter& progress) {
          while (true) {
              std::pair<uint64_t, MI_Type*>* output = nullptr;
              if (output_queue.try_pop(output)) {
                  uint64_t seq_length = output->first;
                  MI_Type* minmers = output->second;
                  for (const auto& mi : *minmers) {
                      if (minmerPosLookupIndex[mi.hash].size() == 0 
                          || minmerPosLookupIndex[mi.hash].back().hash != mi.hash 
                          || minmerPosLookupIndex[mi.hash].back().pos != mi.wpos)
                      {
                          minmerPosLookupIndex[mi.hash].push_back(IntervalPoint {mi.wpos, mi.hash, mi.seqId, side::OPEN});
                          minmerPosLookupIndex[mi.hash].push_back(IntervalPoint {mi.wpos_end, mi.hash, mi.seqId, side::CLOSE});
                      } else {
                          minmerPosLookupIndex[mi.hash].back().pos = mi.wpos_end;
                      }
                  }
                  //this->minmerIndex.insert(this->minmerIndex.end(), minmers->begin(), minmers->end());
                  this->minmerIndex.insert(
                      this->minmerIndex.end(), 
                      std::make_move_iterator(minmers->begin()), 
                      std::make_move_iterator(minmers->end()));

                  // Update progress meter
                  progress.increment(seq_length);
                  
                  delete output->second;
                  delete output;
              } else if (workers_done.load() && output_queue.was_empty()) {
                  break;
              } else {
                  std::this_thread::sleep_for(std::chrono::milliseconds(10));
              }
          }

          // Finalize progress meter
          progress.finish();
      }

      /**
       * @brief     Get sequence metadata and optionally build the sketch table
       *
       * @details   Iterate through ref sequences to get metadata and
       *            optionally compute and save minmers from the reference sequence(s)
       *            assuming a fixed window size
       * @param     compute_seeds   Whether to compute seeds or just collect metadata
       * @param     target_ids      Set of target sequence IDs to sketch over
       */
      void build(bool compute_seeds, const std::vector<std::string>& target_names = {})
      {
        std::chrono::time_point<std::chrono::system_clock> t0 = skch::Time::now();

        if (compute_seeds) {
          // Calculate total sequence length from id manager
          uint64_t total_seq_length = 0;
          for (const auto& seqName : target_names) {
              seqno_t seqId = idManager.getSequenceId(seqName);
              total_seq_length += idManager.getSequenceLength(seqId);
          }

          // Log file processing before initializing progress meter
          for (const auto& fileName : param.refSequences) {
            std::cerr << "[mashmap::skch::Sketch::build] Processing file: " << fileName << std::endl;
          }

          // Initialize progress meter with known total
          progress_meter::ProgressMeter progress(
              total_seq_length,
              "[mashmap::skch::Sketch::build] computing sketch");

          //Create the thread pool 
          ThreadPool<InputSeqContainer, MI_Type> threadPool([this](InputSeqContainer* e) { return buildHelper(e); }, param.threads);

          size_t totalSeqProcessed = 0;
          size_t totalSeqSkipped = 0;
          size_t shortestSeqLength = std::numeric_limits<size_t>::max();
          uint64_t processedBases = 0;
          const uint64_t progressUpdateInterval = 10000; // 10kbp
          
          for (const auto& fileName : param.refSequences) {
            seqiter::for_each_seq_in_file(
              fileName,
              target_names,
              [&](const std::string& seq_name, const std::string& seq) {
                  if (seq.length() >= param.segLength) {
                      seqno_t seqId = idManager.getSequenceId(seq_name);
                      threadPool.runWhenThreadAvailable(new InputSeqContainer(seq, seq_name, seqId));
                      totalSeqProcessed++;
                      shortestSeqLength = std::min(shortestSeqLength, seq.length());

                      //Collect output if available
                      while (threadPool.outputAvailable()) {
                          auto output = threadPool.popOutputWhenAvailable();
                          this->buildHandleThreadOutput(output);
                          processedBases += output->size();
                          // Update progress every 10kbp
                          if (processedBases >= progressUpdateInterval) {
                              progress.increment(processedBases);
                              processedBases = 0;
                          }
                      }
                  } else {
                      totalSeqSkipped++;
                      std::cerr << "WARNING, skch::Sketch::build, skipping short sequence: " << seq_name 
                               << " (length: " << seq.length() << ")" << std::endl;
                  }
              });
          }

          //Collect remaining output objects
          while (threadPool.running()) {
            auto output = threadPool.popOutputWhenAvailable();
            this->buildHandleThreadOutput(output);
            processedBases += output->size();
          }

          // Update progress with any remaining bases
          if (processedBases > 0) {
              progress.increment(processedBases);
          }

          progress.finish();

          std::cerr << "[mashmap::skch::Sketch::build] Total sequences processed: " << totalSeqProcessed << std::endl;
          std::cerr << "[mashmap::skch::Sketch::build] Total sequences skipped: " << totalSeqSkipped << std::endl;
          std::cerr << "[mashmap::skch::Sketch::build] Total sequence length: " << total_seq_length << std::endl;
          std::cerr << "[mashmap::skch::Sketch::build] Unique minmer hashes before pruning = " 
                    << minmerPosLookupIndex.size() << std::endl;
          std::cerr << "[mashmap::skch::Sketch::build] Total minmer windows before pruning = " 
                    << minmerIndex.size() << std::endl;
        }

        std::chrono::duration<double> timeRefSketch = skch::Time::now() - t0;
        std::cerr << "[mashmap::skch::Sketch::build] time spent computing the reference index: " 
                  << timeRefSketch.count() << " sec" << std::endl;

        if (this->minmerIndex.size() == 0)
        {
          std::cerr << "[mashmap::skch::Sketch::build] ERROR, reference sketch is empty. "
                    << "Reference sequences shorter than the kmer size are not indexed" << std::endl;
          exit(1);
        }
      }

      public:

      /**
       * @brief               function to compute minmers given input sequence object
       * @details             this function is run in parallel by multiple threads
       * @param[in]   input   input read details
       * @return              output object containing the mappings
       */
      MI_Type* buildHelper(InputSeqContainer *input)
      {
        MI_Type* thread_output = new MI_Type();

        //Compute minmers in reference sequence
        skch::CommonFunc::addMinmers(
                *thread_output, 
                &(input->seq[0u]), 
                input->len, 
                param.kmerSize, 
                param.segLength, 
                param.alphabetSize, 
                param.sketchSize,
                input->seqId);

        return thread_output;
      }

      /**
       * @brief                 routine to handle thread's local minmer index
       * @param[in] output      thread local minmer output
       */
      void buildHandleThreadOutput(MI_Type* contigMinmerIndex)
      {
        for (MinmerInfo& mi : *contigMinmerIndex)
        {
          if (minmerPosLookupIndex[mi.hash].size() == 0 
                  || minmerPosLookupIndex[mi.hash].back().hash != mi.hash 
                  || minmerPosLookupIndex[mi.hash].back().pos != mi.wpos)
            {
              minmerPosLookupIndex[mi.hash].push_back(IntervalPoint {mi.wpos, mi.hash, mi.seqId, side::OPEN});
              minmerPosLookupIndex[mi.hash].push_back(IntervalPoint {mi.wpos_end, mi.hash, mi.seqId, side::CLOSE});
            } else {
              minmerPosLookupIndex[mi.hash].back().pos = mi.wpos_end;
          }
        }

        this->minmerIndex.insert(
            this->minmerIndex.end(), 
            std::make_move_iterator(contigMinmerIndex->begin()), 
            std::make_move_iterator(contigMinmerIndex->end()));

        delete contigMinmerIndex;
      }


      /**
       * @brief  Write sketch as tsv. TSV indexing is slower but can be debugged easier
       */
      void writeSketchTSV() 
      {
        std::ofstream outStream;
        outStream.open(std::string(param.indexFilename) + ".tsv");
        outStream << "seqId" << "\t" << "strand" << "\t" << "start" << "\t" << "end" << "\t" << "hash\n";
        for (auto& mi : this->minmerIndex) {
          outStream << mi.seqId << "\t" << std::to_string(mi.strand) << "\t" << mi.wpos << "\t" << mi.wpos_end << "\t" << mi.hash << "\n";
        }
        outStream.close(); 
      }


      /**
       * @brief  Write sketch for quick loading
       */
      void writeSketchBinary(std::ofstream& outStream) 
      {
        typename MI_Type::size_type size = minmerIndex.size();
        outStream.write((char*)&size, sizeof(size));
        outStream.write((char*)&minmerIndex[0], minmerIndex.size() * sizeof(MinmerInfo));
      }

      /**
       * @brief  Write posList for quick loading
       */
      void writePosListBinary(std::ofstream& outStream) 
      {
        typename MI_Map_t::size_type size = minmerPosLookupIndex.size();
        outStream.write((char*)&size, sizeof(size));

        for (auto& [hash, ipVec] : minmerPosLookupIndex) 
        {
          MinmerMapKeyType key = hash;
          outStream.write((char*)&key, sizeof(key));
          typename MI_Type::size_type size = ipVec.size();
          outStream.write((char*)&size, sizeof(size));
          outStream.write((char*)&ipVec[0], ipVec.size() * sizeof(MinmerMapValueType::value_type));
        }
      }


      /**
       * @brief  Write posList for quick loading
       */
      // Removed writeFreqKmersBinary function


      /**
       * @brief Write parameters 
       */
      void writeParameters(std::ofstream& outStream)
      {
        // Write segment length, sketch size, and kmer size
        outStream.write((char*) &param.segLength, sizeof(param.segLength));
        outStream.write((char*) &param.sketchSize, sizeof(param.sketchSize));
        outStream.write((char*) &param.kmerSize, sizeof(param.kmerSize));
      }


      /**
       * @brief  Write all index data structures to disk
       */
      void writeIndex(const std::vector<std::string>& target_subset, const std::string& filename = "", bool append = false) 
      {
        fs::path indexFilename = filename.empty() ? fs::path(param.indexFilename) : fs::path(filename);
        std::ofstream outStream;
        if (append) {
            outStream.open(indexFilename, std::ios::binary | std::ios::app);
        } else {
            outStream.open(indexFilename, std::ios::binary);
        }
        if (!outStream) {
            std::cerr << "Error: Unable to open index file for writing: " << indexFilename << std::endl;
            exit(1);
        }
        writeSubIndexHeader(outStream, target_subset);
        writeParameters(outStream);
        writeSketchBinary(outStream);
        writePosListBinary(outStream);
        // Removed writeFreqKmersBinary call
        outStream.close();
      }

      void writeSubIndexHeader(std::ofstream& outStream, const std::vector<std::string>& target_subset) 
      {
        const uint64_t magic_number = 0xDEADBEEFCAFEBABE;
        outStream.write(reinterpret_cast<const char*>(&magic_number), sizeof(magic_number));
        uint64_t num_sequences = target_subset.size();
        outStream.write(reinterpret_cast<const char*>(&num_sequences), sizeof(num_sequences));
        for (const auto& seqName : target_subset) {
            uint64_t name_length = seqName.size();
            outStream.write(reinterpret_cast<const char*>(&name_length), sizeof(name_length));
            outStream.write(seqName.c_str(), name_length);
        }
      }

      /**
       * @brief Read sketch from TSV file
       */
      void readSketchTSV() 
      {
        io::CSVReader<5, io::trim_chars<' '>, io::no_quote_escape<'\t'>> inReader(std::string(param.indexFilename) + ".tsv");
        inReader.read_header(io::ignore_missing_column, "seqId", "strand", "start", "end", "hash");
        hash_t hash;
        offset_t start, end;
        strand_t strand;
        seqno_t seqId;
        while (inReader.read_row(seqId, strand, start, end, hash))
        {
          this->minmerIndex.push_back(MinmerInfo {hash, start, end, seqId, strand});
        }
      }

      /**
       * @brief Read sketch from binary file
       */
      void readSketchBinary(std::ifstream& inStream) 
      {
        typename MI_Type::size_type size = 0;
        inStream.read((char*)&size, sizeof(size));
        minmerIndex.resize(size);
        inStream.read((char*)&minmerIndex[0], minmerIndex.size() * sizeof(MinmerInfo));
      }

      /**
       * @brief  Save posList for quick reading
       */
      void readPosListBinary(std::ifstream& inStream) 
      {
        typename MI_Map_t::size_type numKeys = 0;
        inStream.read((char*)&numKeys, sizeof(numKeys));
        minmerPosLookupIndex.reserve(numKeys);

        for (auto idx = 0; idx < numKeys; idx++) 
        {
          MinmerMapKeyType key = 0;
          inStream.read((char*)&key, sizeof(key));
          typename MinmerMapValueType::size_type size = 0;
          inStream.read((char*)&size, sizeof(size));

          minmerPosLookupIndex[key].resize(size);
          inStream.read((char*)&minmerPosLookupIndex[key][0], size * sizeof(MinmerMapValueType::value_type));
        }
      }


      /**
       * @brief  Read parameters and compare to CLI params
       */
      void readParameters(std::ifstream& inStream)
      {
        decltype(param.segLength) index_segLength;
        decltype(param.sketchSize) index_sketchSize;
        decltype(param.kmerSize) index_kmerSize;

        inStream.read((char*) &index_segLength, sizeof(index_segLength));
        inStream.read((char*) &index_sketchSize, sizeof(index_sketchSize));
        inStream.read((char*) &index_kmerSize, sizeof(index_kmerSize));

        if (param.segLength != index_segLength 
            || param.sketchSize != index_sketchSize
            || param.kmerSize != index_kmerSize)
        {
          std::cerr << "[mashmap::skch::Sketch::readParameters] ERROR: Parameters of indexed sketch differ from current parameters" << std::endl;
          std::cerr << "[mashmap::skch::Sketch::readParameters] Index --> segLength=" << index_segLength
                    << " sketchSize=" << index_sketchSize << " kmerSize=" << index_kmerSize << std::endl;
          std::cerr << "[mashmap::skch::Sketch::readParameters] Current --> segLength=" << param.segLength
                    << " sketchSize=" << param.sketchSize << " kmerSize=" << param.kmerSize << std::endl;
          exit(1);
        }
      }


      /**
       * @brief  Read all index data structures from file
       */
      void readIndex(std::ifstream& inStream, const std::vector<std::string>& targetSequenceNames) 
      {
        std::cerr << "[mashmap::skch::Sketch::readIndex] Reading index" << std::endl;
        if (!readSubIndexHeader(inStream, targetSequenceNames)) {
            std::cerr << "Error: Sequences in the index do not match the expected target sequences." << std::endl;
            exit(1);
        }
        readParameters(inStream);
        readSketchBinary(inStream);
        readPosListBinary(inStream);
        // Removed readFreqKmersBinary call
      }

      bool readSubIndexHeader(std::ifstream& inStream, const std::vector<std::string>& targetSequenceNames) 
      {
        uint64_t magic_number = 0;
        inStream.read(reinterpret_cast<char*>(&magic_number), sizeof(magic_number));
        if (magic_number != 0xDEADBEEFCAFEBABE) {
            std::cerr << "Error: Invalid magic number in index file." << std::endl;
            exit(1);
        }
        uint64_t num_sequences = 0;
        inStream.read(reinterpret_cast<char*>(&num_sequences), sizeof(num_sequences));
        std::vector<std::string> sequenceNames;
        for (uint64_t i = 0; i < num_sequences; ++i) {
            uint64_t name_length = 0;
            inStream.read(reinterpret_cast<char*>(&name_length), sizeof(name_length));
            std::string seqName(name_length, '\0');
            inStream.read(&seqName[0], name_length);
            sequenceNames.push_back(seqName);
        }
        
        return sequenceNames == targetSequenceNames;
      }


      public:

      /**
       * @brief                 check if iterator points to index end
       * @param[in]   iterator
       * @return                boolean value
       */
      bool isMinmerIndexEnd(const MIIter_t &it) const
      {
        return it == this->minmerIndex.end();
      }

      /**
       * @brief     Return end iterator on minmerIndex
       */
      MIIter_t getMinmerIndexEnd() const
      {
        return this->minmerIndex.end();
      }

      void clear()
      {
        minmerPosLookupIndex.clear();
        minmerIndex.clear();
        minmerFreqHistogram.clear();
      }

    }; //End of class Sketch
} //End of namespace skch

#endif
