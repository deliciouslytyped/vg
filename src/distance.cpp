#include "distance.hpp"
using namespace std;
namespace vg {

    //////////  Helper Functions

    size_t snarlMatrixIndex (unordered_map<int, size_t>* indices, 
                                                int start, int end) {
        //Get the index of dist from start to end in a snarl distance matrix
        unordered_map<int, size_t> map = *indices;
        size_t i1 = map[start];
        size_t i2 = map[end];
        size_t length = map.size();
        return (length + 1) * (length / 2) + (i1+1)* (i1/2) + i2 - 1;
    };
   
    int64_t snarlDistance(SnarlDistances* sd,int n1, int n2){
        //Distance between two nodes n1 and n2 in snarl
        //nodes are node id- negative if reverse, positive if forward
        //TODO: the distance index is passed as an argument for now
        size_t i = snarlMatrixIndex(sd->visit_to_index, n1, n2);
        vector<int64_t> distances = *sd->distances;
        return distances[i]; 
    };
   
    int64_t chainDistance(ChainDistances* cd, int v1, int v2) {
        //Distance between v1 and v2 in a chain
        unordered_map<id_t, size_t> snarl_to_index = *cd->snarl_to_index;
        size_t i1 = snarl_to_index[v1];
        size_t i2 = snarl_to_index[v2]; 
        vector<int64_t> prefix_sum = *cd->prefix_sum;
        return abs(prefix_sum[i1] - prefix_sum[i2]);
    };

    int64_t chainLength(DistanceIndex* di, Chain* chain) {
        //Get the length of a chain including length of last node
        id_t c = get_start_of(*chain).snarl().start().node_id();
        unordered_map<id_t, ChainDistances*> cd_map = di->cd;
        vector<int64_t>* prefix_sum = cd_map[c]->prefix_sum;
        vector<int64_t> prefix = *prefix_sum;
        return prefix.back();
    };





    /////////   Create the distance index

    DistanceIndex makeDistanceIndex(VG graph, SnarlManager sm) {
        //Create the distance index given a VG and snarl manager
        DistanceIndex distances;
        return distances;
    };

    bool cmp (pair<const handle_t*, int64_t> x, pair<const handle_t*, int64_t> y) {
                 return (x.second < y.second); 
    };
    int64_t calculateIndex(DistanceIndex* di, VG* graph, SnarlManager& sm, 
                 const Chain* chainp) {
        //Comparison of two pairs of <Node, distance> for priority queue
        Chain chain = *chainp; 
        vector<int64_t> chain_prefix_sum (1,0); //initialize to [0]
        unordered_map<id_t, size_t> chain_snarl_to_index;
        for (int i = 0; i < chain.size(); i++) {
            
            const Snarl snarl = *chain[i];
            //for each snarl in the chain TODO ChainIterator?
            chain_snarl_to_index.insert( make_pair<int, size_t>
                          (snarl.start().node_id(), chain_prefix_sum.size()-1));
            //Initialize components of SnarlDistances struct
            //TODO use visit as key to identify a node+direction? or node id? 
            unordered_map<int, size_t> snarl_visit_to_index;
            pair<unordered_set<Node*>, unordered_set<Edge*>> contents = 
                            sm.shallow_contents(&snarl, *graph, true);
            unordered_set<Node*> allNodes = contents.first;
            //all nodes in snarl includes both boundary nodes of child snarls
            /*TODO better way of getting all the nodes in the snarl? Currently
              don't include end boundary node of a child snarl in distance 
              matrix, maybe should?
            */ 
            //Assign all nodes+direction in snarl to an index
            size_t snarlIndex = 0;
            for (unordered_set<Node*>::iterator n = allNodes.begin(); 
                    n != allNodes.end(); ++n) {
                const Node* node = *n;
                id_t nID = node->id();
                snarl_visit_to_index.insert(make_pair<int, size_t> 
                               (nID, snarlIndex++));
                snarl_visit_to_index.insert(make_pair<int, size_t> 
                               (-nID, snarlIndex++));
            }
            int size = snarl_visit_to_index.size();
            vector<int64_t> snarl_distances ((size+1) * size / 2,-1);
            SnarlDistances sd;
            sd.visit_to_index = &snarl_visit_to_index;
            sd.distances = &snarl_distances;
            di->sd.insert(make_pair<id_t, SnarlDistances*>
                                             (snarl.start().node_id(),&sd));
            
            //Create a NetGraph for current snarl
            NetGraph ng = NetGraph(snarl.start(), snarl.end(), sm.chains_of(&snarl), graph);
            //TODO make a netgraph - need child chains??
            
            // For each node in snarl calculate distance to every reachable node
            for (unordered_set<Node*>::iterator n = allNodes.begin(); 
                    n != allNodes.end(); ++n){
                Node* startNode = *n;
                bool bools [2] = {true, false};
                for (bool rev : bools) {
                int64_t startID; 
                //TODO Better way to loop over all nodes/direction??
                if (rev) { startID = -startNode->id(); }//reverse
                else     { startID = startNode->id(); }
                const handle_t handle = ng.get_handle(startNode->id(), rev);
                priority_queue<pair<const handle_t*, int64_t>, vector<pair<const handle_t*, int64_t>>, decltype(&cmp)> reachable(&cmp);
                pair<const handle_t*, int64_t> init (&handle, 0);
                reachable.push(init);
                while (reachable.size() > 0) {
                    pair<const handle_t*, int64_t> next = reachable.top();
                    reachable.pop();
                    handle_t currHandle = *next.first;
                    int64_t currDist = next.second;
                    int currID;
                    if (ng.get_is_reverse(currHandle)) {
                        currID = -ng.get_id(currHandle);
                    } else {
                        currID = ng.get_id(currHandle);
                    }
 
                    if ( snarlDistance(&sd, startID, currID) == -1) {
                        //If node has not already been found:
   
                        //Get the length of the current node
                        size_t index =  snarlMatrixIndex (&snarl_visit_to_index, 
                                                         startID, currID);
                        snarl_distances.insert(snarl_distances.begin() + index,
                                                  currDist);
                        int64_t nodeLen;
                        if (abs(currID) != snarl.start().node_id() &&
                                                     ng.is_child(currHandle)){
                            //If a child snarl/chain begins at the current node
                            const Snarl* currSnarl = sm.into_which_snarl(abs(currID), 
                                               (currID < 0));
                            if (sm.in_nontrivial_chain(currSnarl)) {//Chain
                                /*TODO assuming start and end are consistent 
                                  within the graph, not relative to the current
                                  orientation
                                */
                                const Chain* currChain = sm.chain_of(currSnarl);
                                unordered_map<id_t, ChainDistances*>::iterator 
                                     chainDists = di->cd.find(
                                       get_start_of(*currChain).snarl().start().node_id());
                                if (chainDists != di->cd.end()) {
                                    //Length of chain has already been found
                                    nodeLen = chainDists->second->prefix_sum->back();
                                    //last element should be length of chain
                                } else {//haven't recursed on this chain yet
                                    nodeLen = calculateIndex( 
                                                     di, graph, sm, currChain);
                                }
                            } else {//Snarl
                                unordered_map<id_t, SnarlDistances*>::iterator 
                                     snarlDists = di->sd.find(currID);
                                if (snarlDists != di->sd.end()) {//Already found
                                    nodeLen = snarlDistance(snarlDists->second,
                                            currSnarl->start().node_id(),
                                         currSnarl->end().node_id());
                                } else {//Haven't recursed on snarl yet
                                    Chain currChain;
                                    currChain.push_back(&snarl);
                                    nodeLen = calculateIndex( 
                                                     di, graph, sm, &currChain);
                                }
                            }
                        } else { //Node is just a node
                            nodeLen = ng.get_length(currHandle);
                            //TODO This doesn't work?
                        }

                        //Add next nodes to priority queue
                        vector<const handle_t*> nextNodes;//vector of adjacent nodes
                        auto addHandle = [&](const handle_t& h)-> bool {
                            nextNodes.push_back(&h);
                            return true;
                        };
                        ng.follow_edges(currHandle, false, addHandle);
                        for (int i = 0; i < nextNodes.size(); i++) {
                            const handle_t* h = nextNodes[i]; 
                            pair<const handle_t*, int64_t> p = 
                                              make_pair (h, currDist + nodeLen);
                            reachable.push(p);
                        }
                    } 
                }//End while loop
            }}//End for loop over starting node/directions
            /* add length of snarl (start of start node to start of end node) to
               the chain prefix sum vector */
            int64_t dist = snarlDistance(&sd, snarl.start().node_id(), 
                                 snarl.end().node_id());
            chain_prefix_sum.push_back(chain_prefix_sum.back() + dist);
        }//End for loop over snarls in chain
        //Add the length of the last node in chain to get length of entire chain
        Visit lastVisit = get_end_of(chain).snarl().end();
        //TODO Visit points <-, snarl as well???
        Node* lastNode = graph->node_by_id[lastVisit.node_id()];
        
        chain_prefix_sum.push_back(chain_prefix_sum.back() + 
               lastNode->sequence().size());
 
        if (chain_prefix_sum.size() > 2) { //If chain and not just one snarl
            ChainDistances cd;
            cd.snarl_to_index = &chain_snarl_to_index;
            cd.prefix_sum = &chain_prefix_sum;
            pair <id_t, ChainDistances*> p = 
                                make_pair(get_start_of(chain).node_id(), &cd);
            di->cd.insert(p);
        }
        return chain_prefix_sum.back();//return length of entire chain
    };

    ////////    Calculate distances

}
