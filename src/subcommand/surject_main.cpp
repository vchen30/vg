// surject_main.cpp: define the "vg surject" subcommand, which forces alignments into linear space

#include <omp.h>
#include <unistd.h>
#include <getopt.h>

#include <string>
#include <vector>
#include <set>

#include "subcommand.hpp"

#include "../vg.hpp"
#include "../stream.hpp"
#include "../utility.hpp"
#include "../mapper.hpp"

using namespace std;
using namespace vg;
using namespace vg::subcommand;

void help_surject(char** argv) {
    cerr << "usage: " << argv[0] << " surject [options] <aln.gam> >[proj.cram]" << endl
         << "Transforms alignments to be relative to particular paths." << endl
         << endl
         << "options:" << endl
         << "    -x, --xg-name FILE      use the graph in this xg index" << endl
         << "    -t, --threads N         number of threads to use" << endl
         << "    -p, --into-path NAME    surject into this path (many allowed, default: all in xg)" << endl
         << "    -F, --into-paths FILE   surject into nonoverlapping path names listed in FILE (one per line)" << endl
         << "    -n, --context-depth N   expand this many steps when collecting graph for surjection (default: 3)" << endl
         << "    -b, --bam-output        write BAM to stdout" << endl
         << "    -s, --sam-output        write SAM to stdout" << endl
         << "    -C, --compression N     level for compression [0-9]" << endl;
}

int main_surject(int argc, char** argv) {

    if (argc == 2) {
        help_surject(argv);
        return 1;
    }

    string xg_name;
    set<string> path_names;
    string path_prefix;
    string path_file;
    string output_type = "gam";
    string input_type = "gam";
    string header_file;
    int compress_level = 9;
    int context_depth = 3;

    int c;
    optind = 2; // force optind past command positional argument
    while (true) {
        static struct option long_options[] =
        {
            {"help", no_argument, 0, 'h'},
            {"xb-name", required_argument, 0, 'x'},
            {"threads", required_argument, 0, 't'},
            {"into-path", required_argument, 0, 'p'},
            {"into-paths", required_argument, 0, 'F'},
            {"into-prefix", required_argument, 0, 'P'},
            {"cram-output", no_argument, 0, 'c'},
            {"bam-output", no_argument, 0, 'b'},
            {"sam-output", no_argument, 0, 's'},
            {"header-from", required_argument, 0, 'H'},
            {"compress", required_argument, 0, 'C'},
            {"context-depth", required_argument, 0, 'n'},
            {0, 0, 0, 0}
        };

        int option_index = 0;
        c = getopt_long (argc, argv, "hx:p:F:P:cbsH:C:t:n:",
                long_options, &option_index);

        // Detect the end of the options.
        if (c == -1)
            break;

        switch (c)
        {

        case 'x':
            xg_name = optarg;
            break;

        case 'p':
            path_names.insert(optarg);
            break;

        case 'F':
            path_file = optarg;
            break;

        case 'P':
            path_prefix = optarg;
            break;

        case 'H':
            header_file = optarg;
            break;

        case 'c':
            output_type = "cram";
            break;

        case 'b':
            output_type = "bam";
            break;

        case 's':
            compress_level = -1;
            output_type = "sam";
            break;

        case 't':
            omp_set_num_threads(atoi(optarg));
            break;

        case 'C':
            compress_level = atoi(optarg);
            break;

        case 'n':
            context_depth = atoi(optarg);
            break;

        case 'h':
        case '?':
            help_surject(argv);
            exit(1);
            break;

        default:
            abort ();
        }
    }

    string file_name = get_input_file_name(optind, argc, argv);

    if (!path_file.empty()){
        // open the file
        ifstream in(path_file);
        string line;
        while (std::getline(in,line)) {
            path_names.insert(line);
        }
    }

    xg::XG* xgidx = nullptr;
    ifstream xg_stream(xg_name);
    if(xg_stream) {
        xgidx = new xg::XG(xg_stream);
    }
    if (!xg_stream || xgidx == nullptr) {
        cerr << "[vg surject] error: could not open xg index" << endl;
        return 1;
    }

    // if no paths were given take all of those in the index
    if (path_names.empty()) {
        for (size_t i = 1; i <= xgidx->path_count; ++i) {
            path_names.insert(xgidx->path_name(i));
        }
    }

    map<string, int64_t> path_length;
    int num_paths = xgidx->max_path_rank();
    for (int i = 1; i <= num_paths; ++i) {
        auto name = xgidx->path_name(i);
        path_length[name] = xgidx->path_length(name);
    }

    int thread_count = get_thread_count();
    vector<Mapper*> mapper;
    mapper.resize(thread_count);
    for (int i = 0; i < thread_count; ++i) {
        Mapper* m = new Mapper(xgidx, nullptr, nullptr);
        m->context_depth = context_depth;
        mapper[i] = m;
    }

    if (input_type == "gam") {
        if (output_type == "gam") {
            int thread_count = get_thread_count();
            vector<vector<Alignment> > buffer;
            buffer.resize(thread_count);
            function<void(Alignment&)> lambda = [&xgidx, &path_names, &buffer, &mapper](Alignment& src) {
                int tid = omp_get_thread_num();
                Alignment surj;
                // Since we're outputting full GAM, we ignore all this info
                // about where on the path the alignment falls. But we need to
                // provide the space to the surject call anyway.
                string path_name;
                int64_t path_pos;
                bool path_reverse;
                buffer[tid].push_back(mapper[tid]->surject_alignment(src, path_names,path_name, path_pos, path_reverse));
                stream::write_buffered(cout, buffer[tid], 100);
            };
            get_input_file(file_name, [&](istream& in) {
                stream::for_each_parallel(in, lambda);
            });
            for (int i = 0; i < thread_count; ++i) {
                stream::write_buffered(cout, buffer[i], 0); // flush
            }
        } else {
            char out_mode[5];
            string out_format = "";
            strcpy(out_mode, "w");
            if (output_type == "bam") { out_format = "b"; }
            else if (output_type == "cram") { out_format = "c"; }
            else { out_format = ""; }
            strcat(out_mode, out_format.c_str());
            if (compress_level >= 0) {
                char tmp[2];
                tmp[0] = compress_level + '0'; tmp[1] = '\0';
                strcat(out_mode, tmp);
            }
            // Define a string to hold the SAM header, to be generated later.
            string header;
            int thread_count = get_thread_count();
            vector<vector<tuple<string, int64_t, bool, Alignment> > > buffer;
            buffer.resize(thread_count);
            map<string, string> rg_sample;

            // bam/sam/cram output
            samFile* out = 0;
            int buffer_limit = 100;

            bam_hdr_t* hdr = NULL;
            int64_t count = 0;
            // TODO: What good is this lock if we continue without getting it if the buffer is overfull???
            omp_lock_t output_lock;
            omp_init_lock(&output_lock);

            // handles buffers, possibly opening the output file if we're on the first record
            auto handle_buffer =
                [&hdr, &header, &path_length, &rg_sample, &buffer_limit,
                &out_mode, &out, &output_lock](vector<tuple<string, int64_t, bool, Alignment> >& buf) {
                    if (buf.size() >= buffer_limit) {
                        // do we have enough data to open the file?
#pragma omp critical (hts_header)
                        {
                            if (!hdr) {
                                hdr = hts_string_header(header, path_length, rg_sample);
                                if ((out = sam_open("-", out_mode)) == 0) {
                                    cerr << "[vg surject] failed to open stdout for writing HTS output" << endl;
                                    exit(1);
                                } else {
                                    // write the header
                                    if (sam_hdr_write(out, hdr) != 0) {
                                        cerr << "[vg surject] error: failed to write the SAM header" << endl;
                                    }
                                }
                            }
                        }
                        // try to get a lock, and force things if we've built up a huge buffer waiting
                        // TODO: Is continuing without the lock safe? And if so why do we have the lock in the first place?
                        if (omp_test_lock(&output_lock) || buf.size() > 10*buffer_limit) {
                            for (auto& s : buf) {
                                auto& path_nom = get<0>(s);
                                auto& path_pos = get<1>(s);
                                auto& path_reverse = get<2>(s);
                                auto& surj = get<3>(s);
                                string cigar = cigar_against_path(surj, path_reverse);
                                bam1_t* b = alignment_to_bam(header,
                                        surj,
                                        path_nom,
                                        path_pos,
                                        path_reverse,
                                        cigar,
                                        "=",
                                        path_pos,
                                        0);
                                int r = 0;
#pragma omp critical (cout)
                                r = sam_write1(out, hdr, b);
                                if (r == 0) { cerr << "[vg surject] error: writing to stdout failed" << endl; exit(1); }
                                bam_destroy1(b);
                            }
                            omp_unset_lock(&output_lock);
                            buf.clear();
                        }
                    }
                };

            function<void(Alignment&)> lambda = [&xgidx,
                                                 &mapper,
                                                 &path_names,
                                                 &path_length,
                                                 &rg_sample,
                                                 &header,
                                                 &out,
                                                 &buffer,
                                                 &count,
                                                 &hdr,
                                                 &out_mode,
                                                 &handle_buffer](Alignment& src) {
                    string path_name;
                    // Make sure to initialize to 0 since it may not be set by
                    // surject_alignment if the read is unmapped, and unmapped
                    // reads need to come out with a 0 1-based position.
                    int64_t path_pos = -1; 
                    bool path_reverse = false;
                    int tid = omp_get_thread_num();
                    auto surj = mapper[tid]->surject_alignment(src, path_names, path_name, path_pos, path_reverse);
                    if (!surj.path().mapping_size()) {
                        surj = src;
                    }
                    // record
                    if (!hdr && !surj.read_group().empty() && !surj.sample_name().empty()) {
#pragma omp critical (hts_header)
                        rg_sample[surj.read_group()] = surj.sample_name();
                    }

                    buffer[tid].push_back(make_tuple(path_name, path_pos, path_reverse, surj));
                    handle_buffer(buffer[tid]);

                };


            // now apply the alignment processor to the stream
            get_input_file(file_name, [&](istream& in) {
                stream::for_each_parallel(in, lambda);
            });
            buffer_limit = 0;
            for (auto& buf : buffer) {
                handle_buffer(buf);
            }
            bam_hdr_destroy(hdr);
            sam_close(out);
            omp_destroy_lock(&output_lock);
        }
    }
    cout.flush();

    return 0;
}

// Register subcommand
static Subcommand vg_surject("surject", "map alignments onto specific paths", main_surject);
