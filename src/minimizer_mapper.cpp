/**
 * \file minimizer_mapper.cpp
 * Defines the code for the minimizer-and-GBWT-based mapper.
 */

#include "minimizer_mapper.hpp"
#include "annotation.hpp"

#include <chrono>
#include <iostream>

namespace vg {

using namespace std;

MinimizerMapper::MinimizerMapper(const xg::XG* xg_index, const gbwt::GBWT* gbwt_index, const MinimizerIndex* minimizer_index,
    SnarlManager* snarl_manager, DistanceIndex* distance_index) :
    xg_index(xg_index), gbwt_index(gbwt_index), minimizer_index(minimizer_index),
    snarl_manager(snarl_manager), distance_index(distance_index), gbwt_graph(*gbwt_index, *xg_index),
    extender(gbwt_graph) {
    
    // Nothing to do!
}

void MinimizerMapper::map(Alignment& aln, AlignmentEmitter& alignment_emitter) {
    // For each input alignment
        
    std::chrono::time_point<std::chrono::system_clock> start = std::chrono::system_clock::now();
        
    // We will find all the seed hits
    vector<pos_t> seeds;
    
    // This will hold all the minimizers in the query
    vector<MinimizerIndex::minimizer_type> minimizers;
    // And either way this will map from seed to minimizer that generated it
    vector<size_t> seed_to_source;
    
    // Find minimizers in the query
    minimizers = minimizer_index->minimizers(aln.sequence());

    size_t rejected_count = 0;
    
    for (size_t i = 0; i < minimizers.size(); i++) {
        // For each minimizer
        if (hit_cap == 0 || minimizer_index->count(minimizers[i].first) <= hit_cap) {
            // The minimizer is infrequent enough to be informative, so feed it into clustering
            
            // Locate it in the graph
            for (auto& hit : minimizer_index->find(minimizers[i].first)) {
                // For each position, remember it and what minimizer it came from
                seeds.push_back(hit);
                seed_to_source.push_back(i);
            }
        } else {
            // The minimizer is too frequent
            rejected_count++;
        }
    }

#ifdef debug
    cerr << "Read " << aln.name() << ": " << aln.sequence() << endl;
    cerr << "Found " << seeds.size() << " seeds from " << (minimizers.size() - rejected_count) << " minimizers, rejected " << rejected_count << endl;
#endif
        
    // Cluster the seeds. Get sets of input seed indexes that go together.
    vector<hash_set<size_t>> clusters = clusterer.cluster_seeds(seeds, distance_limit, *snarl_manager, *distance_index);
    
    // Compute the covered portion of the read represented by each cluster.
    // TODO: Put this and sorting into the clusterer to deduplicate with vg cluster.
    vector<double> read_coverage_by_cluster;
    for (auto& cluster : clusters) {
        // We set bits in here to true when query anchors cover them
        vector<bool> covered(aln.sequence().size());
        // We use this to convert iterators to indexes
        auto start = aln.sequence().begin();
        
        for (auto& hit_index : cluster) {
            // For each hit in the cluster, work out what anchor sequence it is from.
            size_t source_index = seed_to_source.at(hit_index);
            
            for (size_t i = minimizers[source_index].second; i < minimizers[source_index].second + minimizer_index->k(); i++) {
                // Set all the bits in read space for that minimizer.
                // Each minimizr is a length-k exact match starting at a position
                covered[i] = true;
            }
        }
        
        // Count up the covered positions
        size_t covered_count = 0;
        for (auto bit : covered) {
            covered_count += bit;
        }
        
        // Turn that into a fraction
        read_coverage_by_cluster.push_back(covered_count / (double) covered.size());
    }

#ifdef debug
    cerr << "Found " << clusters.size() << " clusters" << endl;
#endif
    
    // Make a vector of cluster indexes to sort
    vector<size_t> cluster_indexes_in_order;
    for (size_t i = 0; i < clusters.size(); i++) {
        cluster_indexes_in_order.push_back(i);
    }

    // Put the most covering cluster's index first
    std::sort(cluster_indexes_in_order.begin(), cluster_indexes_in_order.end(), [&](const size_t& a, const size_t& b) -> bool {
        // Return true if a must come before b, and false otherwise
        return read_coverage_by_cluster.at(a) > read_coverage_by_cluster.at(b);
    });
    
    // We will fill this with the output alignments (primary and secondaries) in score order.
    vector<Alignment> aligned;
    aligned.reserve(cluster_indexes_in_order.size());
    
    // Annotate the original read with metadata before copying
    if (!sample_name.empty()) {
        aln.set_sample_name(sample_name);
    }
    if (!read_group.empty()) {
        aln.set_read_group(read_group);
    }
    
    for (size_t i = 0; i < max(min(max_alignments, cluster_indexes_in_order.size()), (size_t)1); i++) {
        // For each output alignment we will produce (always at least 1,
        // and possibly up to our alignment limit or the cluster count)
        
        // Produce an output Alignment
        aligned.emplace_back(aln);
        Alignment& out = aligned.back();
        // Clear any old refpos annotation
        out.clear_refpos();
        
        if (i < clusters.size()) {
            // We have a cluster; it actually mapped

#ifdef debug
            cerr << "Cluster " << cluster_indexes_in_order[i] << " rank " << i << ": " << endl;
#endif
        
            // For each cluster
            hash_set<size_t>& cluster = clusters[cluster_indexes_in_order[i]];
            
            // Pack the seeds into (read position, graph position) pairs.
            vector<pair<size_t, pos_t>> seed_matchings;
            seed_matchings.reserve(cluster.size());
            for (auto& seed_index : cluster) {
                // For each seed in the cluster, generate its matching pair
                seed_matchings.emplace_back(minimizers[seed_to_source[seed_index]].second, seeds[seed_index]);
#ifdef debug
                cerr << "Seed read:" << minimizers[seed_to_source[seed_index]].second << " = " << seeds[seed_index]
                    << " from minimizer " << seed_to_source[seed_index] << "(" << minimizer_index->count(minimizers[seed_to_source[seed_index]].first) << ")" << endl;
#endif
            }
            
            // Extend seed hits in the cluster into a real alignment path and mismatch count.
            std::pair<Path, size_t> extended = extender.extend_seeds(seed_matchings, aln.sequence());
            auto& path = extended.first;
            auto& mismatch_count = extended.second;

#ifdef debug
            cerr << "Produced path with " << path.mapping_size() << " mappings and " << mismatch_count << " mismatches" << endl;
#endif

            if (path.mapping_size() != 0) {
                // We have a mapping
                
                // Compute a score based on the sequence length and mismatch count.
                // Alignments will only contain matches and mismatches.
                int alignment_score = default_match * (aln.sequence().size() - mismatch_count) - default_mismatch * extended.second;
                
                if (path.mapping().begin()->edit_size() != 0 && edit_is_match(*path.mapping().begin()->edit().begin())) {
                    // Apply left full length bonus based on the first edit
                    alignment_score += default_full_length_bonus;
                }
                if (path.mapping().rbegin()->edit_size() != 0 && edit_is_match(*path.mapping().rbegin()->edit().rbegin())) {
                    // Apply right full length bonus based on the last edit
                    alignment_score += default_full_length_bonus;
                }
               
                // Compute identity from mismatch count.
                double identity = aln.sequence().size() == 0 ? 0.0 : (aln.sequence().size() - mismatch_count) / (double) aln.sequence().size();
                
                // Fill in the extension info
                *out.mutable_path() = path;
                out.set_score(alignment_score);
                out.set_identity(identity);
                
                // Read mapped successfully!
                continue;
            } else {
                // We need to generate some sub-full-length, maybe-extended seeds.
                vector<pair<Path, size_t>> extended_seeds;

                for (const size_t& seed_index : cluster) {
                    // TODO: Until Jouni implements the extender, we just make each hit a 1-base "extension"
                    
                    // Turn the pos_t into a Path
                    Path extended;
                    Mapping* m = extended.add_mapping();
                    *m->mutable_position() = make_position(seeds[seed_index]);
                    Edit* e = m->add_edit();
                    e->set_from_length(1);
                    e->set_to_length(1);

                    // Pair up the path with the read base it is supposed to be mapping
                    extended_seeds.emplace_back(std::move(extended), minimizers[seed_to_source[seed_index]].second);
                }

                // Then we need to find all the haplotypes between each pair of seeds that can connect.
                // We accomplish that by working out the maximum detectable gap using the code in the mapper,
                // and then trace that far through all haplotypes from each extension.
                size_t max_detectable_gap = get_aligner()->longest_detectable_gap(aln); 
                

                // Then we will align against all those haplotype sequences, take the top n, and use them as plausible connecting paths in a MultipathAlignment.
                // Then we take the best linearization of the full MultipathAlignment.

    
            }
        }
        
        // If we get here, either there was no cluster or the cluster produced no extension
        
        // Read was not able to be mapped.
        // Make our output alignment un-aligned.
        out.clear_path();
        out.set_score(0);
        out.set_identity(0);
    }
    
    // Sort again by actual score instead of cluster coverage
    std::sort(aligned.begin(), aligned.end(), [](const Alignment& a, const Alignment& b) -> bool {
        // Return true if a must come before b (i.e. it has a larger score)
        return a.score() > b.score();
    });
    
    if (aligned.size() > max_multimaps) {
        // Drop the lowest scoring alignments
        aligned.resize(max_multimaps);
    }
    
    for (size_t i = 0; i < aligned.size(); i++) {
        // For each output alignment in score order
        auto& out = aligned[i];
        
        // Assign primary and secondary status
        out.set_is_secondary(i > 0);
        out.set_mapping_quality(0);
    }
    
    std::chrono::time_point<std::chrono::system_clock> end = std::chrono::system_clock::now();
    std::chrono::duration<double> elapsed_seconds = end-start;
    
    if (!aligned.empty()) {
        // Annotate the primary alignment with mapping runtime
        set_annotation(aligned[0], "map_seconds", elapsed_seconds.count());
    }
    
    // Ship out all the aligned alignments
    alignment_emitter.emit_mapped_single(std::move(aligned));
}


}


