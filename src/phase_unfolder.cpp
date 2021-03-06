#include "phase_unfolder.hpp"
#include "progress_bar.hpp"

#include <cassert>
#include <iostream>
#include <map>

namespace vg {

PhaseUnfolder::PhaseUnfolder(const xg::XG& xg_index, const gbwt::GBWT& gbwt_index, vg::id_t next_node) :
    xg_index(xg_index), gbwt_index(gbwt_index), mapping(next_node) {
    assert(this->mapping.begin() > this->xg_index.get_max_id());
}

void PhaseUnfolder::unfold(VG& graph, bool show_progress) {
    std::list<VG> components = this->complement_components(graph, show_progress);

    size_t haplotype_paths = 0;
    VG unfolded;
    for (VG& component : components) {
        haplotype_paths += this->unfold_component(component, graph, unfolded);
    }
    if (show_progress) {
        std::cerr << "Unfolded graph: "
                  << unfolded.node_count() << " nodes, " << unfolded.edge_count() << " edges on "
                  << haplotype_paths << " paths" << std::endl;
    }

    graph.extend(unfolded);
}

void PhaseUnfolder::restore_paths(VG& graph, bool show_progress) const {

    for (size_t path_rank = 1; path_rank <= this->xg_index.max_path_rank(); path_rank++) {
        const xg::XGPath& path = this->xg_index.get_path(this->xg_index.path_name(path_rank));
        if (path.ids.size() == 0) {
            continue;
        }

        gbwt::node_type prev = gbwt::Node::encode(path.node(0), path.is_reverse(0));
        for (size_t i = 1; i < path.ids.size(); i++) {
            gbwt::node_type curr = gbwt::Node::encode(path.node(i), path.is_reverse(i));
            Edge candidate = make_edge(prev, curr);
            if (!graph.has_edge(candidate)) {
                graph.add_node(this->xg_index.node(candidate.from()));
                graph.add_node(this->xg_index.node(candidate.to()));
                graph.add_edge(candidate);
            }
            prev = curr;
        }
    }

    if (show_progress) {
        std::cerr << "Restored graph: " << graph.node_count() << " nodes, " << graph.edge_count() << " edges" << std::endl;
    }
}

size_t path_size(const xg::XGPath& path) {
    return path.ids.size();
}

size_t path_size(const std::vector<gbwt::node_type>& path) {
    return path.size();
}

vg::id_t path_node(const xg::XGPath& path, size_t i) {
    return path.node(i);
}

vg::id_t path_node(const std::vector<gbwt::node_type>& path, size_t i) {
    return gbwt::Node::id(path[i]);
}

bool path_reverse(const xg::XGPath& path, size_t i) {
    return path.is_reverse(i);
}

bool path_reverse(const std::vector<gbwt::node_type>& path, size_t i) {
    return gbwt::Node::is_reverse(path[i]);
}

struct PathBranch {
    size_t offset;
    size_t curr;    // Branch to choose at offset.
    size_t next;    // Branch to choose at offset + 1.

    void advance() {
        offset++;
        curr = next;
        next = 0;
    }
};

template<class PathType>
bool verify_path(const PathType& path, VG& unfolded, const hash_map<vg::id_t, std::vector<vg::id_t>>& reverse_mapping) {

    if (path_size(path) < 2) {
        return true;
    }

    // For each branching point: (offset, next duplicate at offset, next duplicate at offset + 1).
    // Initialize with all duplicates of the first node.
    std::vector<PathBranch> branches;
    {
        vg::id_t node_id = path_node(path, 0);
        auto iter = reverse_mapping.find(node_id);
        if (iter != reverse_mapping.end()) {
            for (size_t i = 0; i < iter->second.size(); i++) {
                branches.push_back({ 0, i, 0 });
            }
        } else {
            branches.push_back({ 0, 0, 0 });
        }
    }

    // Note that we can discard all unexplored branches every time the graph
    // contains the original node or only one duplicate of the current node.
    while (!branches.empty()) {
        PathBranch branch = branches.back(); branches.pop_back();
        vg::id_t node_id = path_node(path, branch.offset);
        auto iter = reverse_mapping.find(node_id);
        if (iter != reverse_mapping.end()) {
            node_id = iter->second[branch.curr];
        }
        gbwt::node_type curr = gbwt::Node::encode(node_id, path_reverse(path, branch.offset));

        // Extend the next path from the current branch.
        while (branch.offset + 1 < path_size(path)) {
            node_id = path_node(path, branch.offset + 1);
            size_t duplicates = 0;
            iter = reverse_mapping.find(node_id);
            if (iter != reverse_mapping.end()) {
                node_id = iter->second[branch.next];
                duplicates = iter->second.size();
                if (branch.next + 1 < duplicates) {
                    branches.push_back({ branch.offset, branch.curr, branch.next + 1 });
                }
            }
            gbwt::node_type next = gbwt::Node::encode(node_id, path_reverse(path, branch.offset + 1));
            Edge candidate = PhaseUnfolder::make_edge(curr, next);
            if (!unfolded.has_edge(candidate)) {
                break;
            }
            if (duplicates <= 1) {  // All paths corresponding to this must go through the next node.
                branches.clear();
            }
            curr = next;
            branch.advance();
        }
        if (branch.offset + 1 >= path_size(path)) {
            return true;
        }
    }

    return false;
}

size_t PhaseUnfolder::verify_paths(VG& unfolded, bool show_progress) const {

    // Create a mapping from original -> duplicates.
    hash_map<vg::id_t, std::vector<vg::id_t>> reverse_mapping;
    for (gcsa::size_type duplicate = this->mapping.begin(); duplicate < this->mapping.end(); duplicate++) {
        vg::id_t original_id = this->mapping(duplicate);
        reverse_mapping[original_id].push_back(duplicate);
        if (unfolded.has_node(original_id)) {
            reverse_mapping[original_id].push_back(original_id);
        }
    }
    for (auto& mapping : reverse_mapping) {
        gcsa::removeDuplicates(mapping.second, false);
    }

    size_t total_paths = this->xg_index.max_path_rank() + this->gbwt_index.sequences(), verified = 0, failures = 0;
    ProgressBar* progress = nullptr;
    if (show_progress) {
        progress = new ProgressBar(total_paths, "Verifying paths");
        progress->Progressed(verified);
    }

    #pragma omp parallel for schedule(dynamic, 1)
    for (size_t i = 0; i < total_paths; i++) {
        bool successful = true;
        if (i < this->xg_index.max_path_rank()) {
            const xg::XGPath& path = this->xg_index.get_path(this->xg_index.path_name(i + 1));
            successful = verify_path(path, unfolded, reverse_mapping);
        } else {
            std::vector<gbwt::node_type> path = this->gbwt_index.extract(i - this->xg_index.max_path_rank());
            successful = verify_path(path, unfolded, reverse_mapping);
        }
        #pragma omp critical
        {
            if (!successful) {
                failures++;
            }
            verified++;
            if (show_progress) {
                progress->Progressed(verified);
            }
        }
    }

    if (show_progress) {
        delete progress; progress = nullptr;
        std::cerr << std::endl;
    }
    return failures;
}

void PhaseUnfolder::write_mapping(const std::string& filename) const {
    std::ofstream out(filename, std::ios_base::binary);
    if (!out) {
        std::cerr << "[PhaseUnfolder]: cannot create mapping file " << filename << std::endl;
        return;
    }
    this->mapping.serialize(out);
    out.close();
}

void PhaseUnfolder::read_mapping(const std::string& filename) {
    std::ifstream in(filename, std::ios_base::binary);
    if (!in) {
        std::cerr << "[PhaseUnfolder]: cannot open mapping file " << filename << std::endl;
        return;
    }
    this->mapping.load(in);
    in.close();
    assert(this->mapping.begin() > this->xg_index.get_max_id());
}

vg::id_t PhaseUnfolder::get_mapping(vg::id_t node) const {
    return this->mapping(node);
}

std::list<VG> PhaseUnfolder::complement_components(VG& graph, bool show_progress) {
    VG complement;

    // Add missing edges supported by XG paths.
    for (size_t path_rank = 1; path_rank <= this->xg_index.max_path_rank(); path_rank++) {
        const xg::XGPath& path = this->xg_index.get_path(this->xg_index.path_name(path_rank));
        if (path.ids.size() == 0) {
            continue;
        }
        gbwt::node_type prev = gbwt::Node::encode(path.node(0), path.is_reverse(0));
        for (size_t i = 1; i < path.ids.size(); i++) {
            gbwt::node_type curr = gbwt::Node::encode(path.node(i), path.is_reverse(i));
            Edge candidate = make_edge(prev, curr);
            if (!graph.has_edge(candidate)) {
                complement.add_node(this->xg_index.node(candidate.from()));
                complement.add_node(this->xg_index.node(candidate.to()));
                complement.add_edge(candidate);
            }
            prev = curr;
        }
    }

    // Add missing edges supported by GBWT threads.
    for (gbwt::comp_type comp = 1; comp < this->gbwt_index.effective(); comp++) {
        gbwt::node_type gbwt_node = this->gbwt_index.toNode(comp);
        std::vector<gbwt::edge_type> outgoing = this->gbwt_index.edges(gbwt_node);
        for (gbwt::edge_type outedge : outgoing) {
            if (outedge.first == gbwt::ENDMARKER) {
                continue;
            }
            Edge candidate = make_edge(gbwt_node, outedge.first);
            if (!graph.has_edge(candidate)) {
                complement.add_node(this->xg_index.node(candidate.from()));
                complement.add_node(this->xg_index.node(candidate.to()));
                complement.add_edge(candidate);
            }
        }
    }

    std::list<VG> components;
    complement.disjoint_subgraphs(components);
    if (show_progress) {
        std::cerr << "Complement graph: "
                  << complement.node_count() << " nodes, " << complement.edge_count() << " edges in "
                  << components.size() << " components" << std::endl;
    }
    return components;
}

size_t PhaseUnfolder::unfold_component(VG& component, VG& graph, VG& unfolded) {
    // Find the border nodes shared between the component and the graph.
    component.for_each_node([&](Node* node) {
       if (graph.has_node(node->id())) {
           this->border.insert(node->id());
       }
    });

    // Generate the paths starting from each border node.
    for (vg::id_t start_node : this->border) {
        this->generate_paths(component, start_node);
        this->generate_threads(component, start_node);
    }

    auto insert_node = [&](gbwt::node_type node) {
        Node temp = this->xg_index.node(this->get_mapping(gbwt::Node::id(node)));
        temp.set_id(gbwt::Node::id(node));
        unfolded.add_node(temp);
    };

    // Create the unfolded component from the tries.
    for (auto mapping : this->prefixes) {
        gbwt::node_type from = mapping.first.first, to = mapping.second;
        insert_node(from);
        insert_node(to);
        unfolded.add_edge(make_edge(from, to));
    }
    for (auto mapping : this->suffixes) {
        gbwt::node_type from = mapping.second, to = mapping.first.second;
        insert_node(from);
        insert_node(to);
        unfolded.add_edge(make_edge(from, to));
    }
    for (auto edge : this->crossing_edges) {
        insert_node(edge.first);
        insert_node(edge.second);
        unfolded.add_edge(make_edge(edge.first, edge.second));
    }

    size_t haplotype_paths = this->crossing_edges.size();
    this->border.clear();
    this->prefixes.clear();
    this->suffixes.clear();
    this->crossing_edges.clear();
    return haplotype_paths;
}

void PhaseUnfolder::generate_paths(VG& component, vg::id_t from) {

    for (size_t path_rank = 1; path_rank <= this->xg_index.max_path_rank(); path_rank++) {
        const xg::XGPath& path = this->xg_index.get_path(this->xg_index.path_name(path_rank));

        std::vector<size_t> occurrences = this->xg_index.node_ranks_in_path(from, path_rank);
        for (size_t occurrence : occurrences) {
            // Forward.
            {
                gbwt::node_type prev = gbwt::Node::encode(path.node(occurrence), path.is_reverse(occurrence));
                path_type buffer { prev };
                for (size_t i = occurrence + 1; i < path.ids.size(); i++) {
                    gbwt::node_type curr = gbwt::Node::encode(path.node(i), path.is_reverse(i));
                    Edge candidate = make_edge(prev, curr);
                    if (!component.has_edge(candidate)) {
                        break;  // Found a maximal path.
                    }
                    buffer.push_back(curr);
                    if (this->border.find(gbwt::Node::id(curr)) != this->border.end()) {
                        break;  // Found a border-to-border path.
                    }
                    prev = curr;
                }
                this->insert_path(buffer);
            }

            // Backward.
            {
                gbwt::node_type prev = gbwt::Node::encode(path.node(occurrence), !path.is_reverse(occurrence));
                path_type buffer { prev };
                bool found_border = false;
                for (size_t i = occurrence; i > 0 ; i--) {
                    gbwt::node_type curr = gbwt::Node::encode(path.node(i - 1), !path.is_reverse(i - 1));
                    Edge candidate = make_edge(prev, curr);
                    if (!component.has_edge(candidate)) {
                        break;  // Found a maximal path.
                    }
                    buffer.push_back(curr);
                    if (this->border.find(gbwt::Node::id(curr)) != this->border.end()) {
                        break;  // Found a border-to-border path.
                    }
                    prev = curr;
                }
                this->insert_path(buffer);
            }
        }
    }
}

void PhaseUnfolder::generate_threads(VG& component, vg::id_t from) {
    this->create_state(from, false);
    this->create_state(from, true);

    while (!this->states.empty()) {
        state_type state = this->states.top(); this->states.pop();
        vg::id_t node = gbwt::Node::id(state.first.node);
        bool is_reverse = gbwt::Node::is_reverse(state.first.node);

        if (state.second.size() >= 2 && this->border.find(node) != this->border.end()) {
            this->insert_path(state.second);    // Border-to-border path.
            continue;
        }

        std::vector<Edge*> edges = component.edges_of(component.get_node(node));
        bool was_extended = false;
        for (Edge* edge : edges) {
            if (edge->from() == node && edge->from_start() == is_reverse) {
                was_extended |= this->extend_state(state, edge->to(), edge->to_end());
            }
            else if (edge->to() == node && edge->to_end() != is_reverse) {
                was_extended |= this->extend_state(state, edge->from(), !edge->from_start());
            }
        }

        if (!was_extended) {
            this->insert_path(state.second);    // Maximal path.
        }
    }
}

void PhaseUnfolder::create_state(vg::id_t node, bool is_reverse) {
    search_type search = this->gbwt_index.find(gbwt::Node::encode(node, is_reverse));
    if (search.empty()) {
        return;
    }
    this->states.push(std::make_pair(search, path_type {search.node}));
}

bool PhaseUnfolder::extend_state(state_type state, vg::id_t node, bool is_reverse) {
    state.first = this->gbwt_index.extend(state.first, gbwt::Node::encode(node, is_reverse));
    if (state.first.empty()) {
        return false;
    }
    state.second.push_back(state.first.node);
    this->states.push(state);
    return true;
}

void PhaseUnfolder::insert_path(const path_type& path) {

    if (path.size() < 2) {
        return;
    }
    path_type reverse_complement(path.size(), 0);
    for (size_t i = 0; i < path.size(); i++) {
        reverse_complement[path.size() - 1 - i] = gbwt::Node::reverse(path[i]);
    }
    const path_type& to_insert = std::min(path, reverse_complement);

    /*
      Break the path in half. For each prefix / suffix, check if we already
      have a mapping for the next node. If not, create a new duplicate of
      the node and insert the mapping into the corresponding trie. Finally
      create a crossing edge between the full prefix and the full suffix.
    */

    // Prefixes.
    gbwt::node_type from = to_insert.front();
    for (size_t i = 1; i < (to_insert.size() + 1) / 2; i++) {
        std::pair<gbwt::node_type, gbwt::node_type> key(from, to_insert[i]);
        auto iter = this->prefixes.find(key);
        if (iter == this->prefixes.end()) {
            gbwt::size_type new_id = this->mapping.insert(gbwt::Node::id(to_insert[i]));
            from = this->prefixes[key] = gbwt::Node::encode(new_id, gbwt::Node::is_reverse(to_insert[i]));
        } else {
            from = iter->second;
        }
    }

    // Suffixes.
    gbwt::node_type to = to_insert.back();
    for (size_t i = to_insert.size() - 2; i >= (to_insert.size() + 1) / 2; i--) {
        std::pair<gbwt::node_type, gbwt::node_type> key(to_insert[i], to);
        auto iter = this->suffixes.find(key);
        if (iter == this->suffixes.end()) {
            gbwt::size_type new_id = this->mapping.insert(gbwt::Node::id(to_insert[i]));
            to = this->suffixes[key] = gbwt::Node::encode(new_id, gbwt::Node::is_reverse(to_insert[i]));
        } else {
            to = iter->second;
        }
    }

    // Crossing edge.
    this->crossing_edges.insert(std::make_pair(from, to));
}

} 
