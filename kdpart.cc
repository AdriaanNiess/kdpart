// See LICENSE for license details.

#include "kdpart.h"

namespace kdpart {

bool operator==(const PartTreeStorage& a, const PartTreeStorage& b)
{
    return a.inner == b.inner
            && a.pstart == b.pstart
            && a.pend == b.pend
            && a.lu == b.lu
            && a.ro == b.ro
            && a.split_direction == b.split_direction
            && a.split_coord == b.split_coord
            && a.psplit == b.psplit;
}

bool operator!=(const PartTreeStorage& a, const PartTreeStorage& b)
{
    return !(a == b);
}

namespace marshall {

size_t marshall_size(const PartTreeStorage& t)
{
    size_t s = 0;
    t.apply_to_data_vectors([&s](const auto& v){
        using value_type = typename std::remove_reference<decltype(v)>::type::value_type;
        s += v.size() * sizeof(value_type);
    });
    return s;
}

size_t marshall_size_per_node(const PartTreeStorage& t)
{
    size_t s = 0;
    t.apply_to_data_vectors([&s](const auto& v){
        using value_type = typename std::remove_reference<decltype(v)>::type::value_type;
        s += sizeof(value_type);
    });
    return s;
}

std::vector<char> marshall_parttree(const PartTreeStorage& t)
{
    std::vector<char> mdata(marshall_size(t));
    char *it = &mdata[0];

    t.apply_to_data_vectors([&it](const auto& v){
        using value_type = typename std::remove_reference<decltype(v)>::type::value_type;
        size_t nbytes = v.size() * sizeof(value_type);
        std::memcpy(it, v.data(), nbytes);
        it += nbytes;
    });

    return mdata;
}

PartTreeStorage unmarshall_parttree(std::vector<char> mdata)
{
    PartTreeStorage t;
    size_t storage_size = mdata.size() / marshall_size_per_node(t);
    char *it = &mdata[0];

    t.apply_to_data_vectors([storage_size, &it](auto& v){
        using value_type = typename std::remove_reference<decltype(v)>::type::value_type;
        size_t nbytes = storage_size * sizeof(value_type);
        v.resize(storage_size);
        std::memcpy(v.data(), it, nbytes);
        it += nbytes;
    });
    return t;
}

} // namespace marshall

std::pair<int, int> fast_splitting(std::vector<double> loads, int nproc)
{
    const double target_frac = 0.5;
    int nproc1 = target_frac * nproc;
    // Fraction for splitting the payload at approx. half. However, account for odd sizes (frac != 0.5).
    double frac = static_cast<double>(nproc1) / nproc;

    std::partial_sum(loads.begin(), loads.end(), loads.begin());
    decltype(loads)::value_type target_load = frac * loads[loads.size() - 1];
    int where = std::distance(loads.begin(), ::util::find_nearest(loads.begin(), loads.end(), target_load));

    return std::make_pair(where, nproc1);
}

std::pair<int, int> quality_splitting(std::vector<double> loads, int nproc)
{ 
    /** Struct representing a possible splitting for "quality_splitting".
     */
    struct opt_value {
        double comp;   //< Objective function value for optimization
        int where;     //< Point of splitting in "loads"
        int nproc1; //< Number of processes in the first subset
    };

    std::partial_sum(loads.begin(), loads.end(), loads.begin());
    const double maxload = loads[loads.size() - 1];

    // Minimize max(prefix1 / nproc1, prefix2 / procs2)
    std::vector<opt_value> values;
    values.reserve(nproc - 1);
    for (int size1 = 1; size1 < nproc; ++size1) {
        // Find most equal load splitting to size1 vs. nproc-size1 processor splitting
        double frac = static_cast<double>(size1) / nproc;
        decltype(loads)::value_type target_load = frac * maxload;
        auto end1 = ::util::find_nearest(loads.begin(), loads.end(), target_load);

        const int pprefix = size1;
        const int psuffix = nproc - size1;
        const double lprefix = *end1;
        const double lsuffix = maxload - *end1;
        const int where = std::distance(std::begin(loads), end1);

        // HEURISTIK: Verteuere sehr ungleiche Splittings: +1% Kosten für jeden
        // Prozess aus der größeren Teilmenge.
        // Die Annahme hier ist, dass "lsuffix" Last perfekt auf "psuffix"
        // Prozesse aufgeteilt werden kann. Deshalb werden kleine "pprefix"
        // (möglichst nahe an "target_load" liegendem "lprefix") bevorzugt,
        // weil dann der "Überhang" zwischen target_load und dem nächst
        // größeren Element auf mehr Prozesse (nämlich "psuffix" viele)
        // verteilt wird.
        // Die Annahme, dass das perfekt aufgeteilt werden kann, ist aber
        // falsch (auch in der Zukunft wird *diskret* in einer Dimension
        // gesplittet) und das Ganze führt deshalb praktisch immer zu höheren
        // Varianzen in den Gebietskosten als mit "get_quick_splitting".
        // Interessanterweise bleiben die Durchschnittskosten aller Teilgebiete
        // aber durchaus unter denen von "get_quick_splitting".
        //
        // Mit dieser Penalisierung bekommen wir die Varianzen tatsächlich
        // auf dasselbe Niveau wie es mit "get_quick_splitting" erreicht wird.
        //
        // Deshalb penalisieren wir hier große pprefix oder psuffix.
        //
        // Nebeneffekt: Wir erlauben so offenbar (empirisch ermittelt) mehr
        // Prozesse im selben Gebiet, weil tiefere Splittings möglich sind
        // aufgrund der ausgeglicheneren Aufteilung.
        const double comp = std::max(lprefix / pprefix, lsuffix / psuffix) * (1.0 + 0.01 * (std::max(pprefix, psuffix) - std::min(pprefix, psuffix)));
        values.push_back({comp, where, pprefix});
    }
    auto opt = ::util::middle_most_min_element(std::begin(values), std::end(values), [](const opt_value& a, const opt_value& b){
        return a.comp < b.comp;
    });
    
    return std::make_pair(opt->where, opt->nproc1);
}

int linearize(const std::array<int, 3> c, const std::array<int, 3> box)
{
    return (c[0] * box[1] + c[1]) * box[2] + c[2];
}

PartTreeStorage repart_parttree_par(const PartTreeStorage& s, MPI_Comm comm, const std::vector<double>& cellweights)
{
    using cell_type = std::array<int, 3>;
    util::GlobalVector<double> global_load(comm, cellweights);

    // Is only being evaluated on rank 0
    auto global_load_func = [&s, &global_load](const cell_type& c){
        auto n = s.node_of_cell(c);

        // Transform c to process ("rank") local coordinates
        cell_type loc_c, loc_box;
        for (auto i = 0; i < 3; ++i) {
            loc_c[i] = c[i] - n.lu()[i];
            loc_box[i] = n.ro()[i] - n.lu()[i];
        }

        auto i = linearize(loc_c, loc_box);
        assert(global_load.size(n.rank()) > i);
        
        return global_load(n.rank(), i);
    };

    return make_parttree_par(comm, s.root().ro(), global_load_func, quality_splitting);
}

PartTreeStorage initial_part_par(int size, std::array<int, 3> ro)
{
    auto load = [](const auto&) {
        return 1.0;
    };
    return make_parttree(size, {{0, 0, 0}}, ro, load, fast_splitting);
}

} // namespace kdpart
