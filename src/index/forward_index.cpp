/**
 * @file forward_index.cpp
 * @author Sean Massung
 */

#include "analyzers/analyzer.h"
#include "corpus/corpus.h"
#include "corpus/corpus_factory.h"
#include "corpus/libsvm_corpus.h"
#include "cpptoml.h"
#include "index/chunk_reader.h"
#include "index/disk_index_impl.h"
#include "index/forward_index.h"
#include "index/inverted_index.h"
#include "index/metadata_writer.h"
#include "index/postings_file.h"
#include "index/postings_file_writer.h"
#include "index/postings_inverter.h"
#include "index/string_list.h"
#include "index/string_list_writer.h"
#include "index/vocabulary_map.h"
#include "index/vocabulary_map_writer.h"
#include "io/libsvm_parser.h"
#include "logging/logger.h"
#include "parallel/thread_pool.h"
#include "util/disk_vector.h"
#include "util/mapping.h"
#include "util/pimpl.tcc"
#include "util/printing.h"
#include "util/shim.h"
#include "util/time.h"

namespace meta
{
namespace index
{

/**
 * Implementation of a forward_index.
 */
class forward_index::impl
{
  public:
    /**
     * Constructs an implementation based on a forward_index.
     */
    impl(forward_index* idx);

    /**
     * Tokenizes the documents in the corpus in parallel, yielding
     * num_threads number of forward_index chunks that then need to be
     * merged.
     */
    void tokenize_docs(corpus::corpus* corpus,
                       const analyzers::analyzer<double>& analyzer,
                       metadata_writer& mdata_writer, uint64_t ram_budget);

    /**
     * Merges together num_chunks number of intermediate chunks, using the
     * given vocabulary to do the renumbering.
     *
     * The vocabulary mapping will assign ids in insertion order, but we
     * will want our ids in lexicographic order for vocabulary_map to
     * work, so this function will sort the vocabulary and perform a
     * re-numbering of the old ids.
     */
    void merge_chunks(size_t num_chunks, util::probe_set<std::string> vocab);

    /**
     * @param config the configuration settings for this index
     */
    void create_libsvm_postings(const cpptoml::table& config);

    /**
     * @param inv_idx The inverted index to uninvert
     * @param ram_budget The **estimated** allowed size of an in-memory
     * chunk
     */
    void uninvert(const inverted_index& inv_idx, uint64_t ram_budget);

    /**
     * @param name The name of the inverted index to copy data from
     */
    void create_uninverted_metadata(const std::string& name);

    /**
     * @param config the configuration settings for this index
     * @return whether this index will be based off of a single
     * libsvm-formatted corpus file
     */
    bool is_libsvm_format(const cpptoml::table& config) const;

    /**
     * Compresses the postings file created by uninverting.
     * @param filename The file to compress
     * @param num_docs The number of documents in that file
     */
    void compress(const std::string& filename, uint64_t num_docs);

    /**
     * Loads the postings file.
     * @param filename The path to the postings file to load
     */
    void load_postings();

    /// the total number of unique terms if term_id_mapping_ is unused
    uint64_t total_unique_terms_;

    /// the postings file
    util::optional<postings_file<forward_index::primary_key_type,
                                 forward_index::secondary_key_type, double>>
        postings_;

  private:
    /// Pointer to the forward_index this is an implementation of
    forward_index* idx_;
};

forward_index::forward_index(const cpptoml::table& config)
    : disk_index{config, *config.get_as<std::string>("forward-index")},
      fwd_impl_{this}
{
    /* nothing */
}

forward_index::impl::impl(forward_index* idx) : idx_{idx}
{
    /* nothing */
}

forward_index::forward_index(forward_index&&) = default;
forward_index::~forward_index() = default;
forward_index& forward_index::operator=(forward_index&&) = default;

bool forward_index::valid() const
{
    if (!filesystem::file_exists(index_name() + "/corpus.uniqueterms"))
    {
        LOG(info) << "Existing forward index detected as invalid; recreating"
                  << ENDLG;
        return false;
    }
    for (auto& f : impl_->files)
    {
        // this is not required if generated directly from libsvm data
        if (f == impl_->files[TERM_IDS_MAPPING]
            || f == impl_->files[TERM_IDS_MAPPING_INVERSE])
            continue;

        if (!filesystem::file_exists(index_name() + "/" + std::string{f}))
        {
            LOG(info) << "Existing forward index detected as invalid (missing "
                      << f << "); recreating" << ENDLG;
            return false;
        }
    }
    return true;
}

std::string forward_index::liblinear_data(doc_id d_id) const
{
    if (d_id >= num_docs())
        throw forward_index_exception{"invalid doc_id in search_primary"};

    auto pdata = search_primary(d_id);
    std::stringstream out;

    out << lbl_id(d_id);
    for (const auto& count : pdata->counts())
        out << ' ' << (count.first + 1) << ':' << count.second;
    return out.str();
}

void forward_index::load_index()
{
    LOG(info) << "Loading index from disk: " << index_name() << ENDLG;

    impl_->initialize_metadata();
    impl_->load_labels();

    auto config = cpptoml::parse_file(index_name() + "/config.toml");
    if (!fwd_impl_->is_libsvm_format(*config))
        impl_->load_term_id_mapping();

    impl_->load_label_id_mapping();
    fwd_impl_->load_postings();

    std::ifstream unique_terms_file{index_name() + "/corpus.uniqueterms"};
    unique_terms_file >> fwd_impl_->total_unique_terms_;
}

void forward_index::create_index(const cpptoml::table& config)
{
    {
        std::ofstream config_file{index_name() + "/config.toml"};
        config_file << config;
    }

    // if the corpus is a single libsvm formatted file, then we are done;
    // otherwise, we will create an inverted index and the uninvert it
    if (fwd_impl_->is_libsvm_format(config))
    {
        LOG(info) << "Creating index from libsvm data: " << index_name()
                  << ENDLG;

        fwd_impl_->create_libsvm_postings(config);
        impl_->save_label_id_mapping();
    }
    else
    {
        auto ram_budget = static_cast<uint64_t>(
            config.get_as<int64_t>("indexer-ram-budget").value_or(1024));

        if (config.get_as<bool>("uninvert").value_or(false))
        {
            LOG(info) << "Creating index by uninverting: " << index_name()
                      << ENDLG;
            {
                // Ensure all files are flushed before uninverting
                make_index<inverted_index>(config);
            }
            auto inv_idx = make_index<inverted_index>(config);

            fwd_impl_->create_uninverted_metadata(inv_idx->index_name());
            impl_->load_labels();
            // RAM budget is given in MB
            fwd_impl_->uninvert(*inv_idx, ram_budget * 1024 * 1024);
            impl_->load_term_id_mapping();
            fwd_impl_->total_unique_terms_ = impl_->total_unique_terms();
        }
        else
        {
            LOG(info) << "Creating forward index: " << index_name() << ENDLG;

            auto docs = corpus::make_corpus(config);

            {
                auto analyzer = analyzers::load<double>(config);

                metadata_writer mdata_writer{index_name(), docs->size(),
                                             docs->schema()};

                impl_->load_labels(docs->size());

                // RAM budget is given in MB
                fwd_impl_->tokenize_docs(docs.get(), *analyzer, mdata_writer,
                                         ram_budget * 1024 * 1024);
                impl_->load_term_id_mapping();
                fwd_impl_->total_unique_terms_ = impl_->total_unique_terms();

                // reload the label file to ensure it was flushed
                impl_->load_labels();
            }
        }
    }

    impl_->load_label_id_mapping();
    fwd_impl_->load_postings();
    impl_->initialize_metadata();

    {
        std::ofstream unique_terms_file{index_name() + "/corpus.uniqueterms"};
        unique_terms_file << fwd_impl_->total_unique_terms_;
    }

    assert(filesystem::file_exists(index_name() + "/corpus.uniqueterms"));

    LOG(info) << "Done creating index: " << index_name() << ENDLG;
}

void forward_index::impl::tokenize_docs(corpus::corpus* docs,
                                        const analyzers::analyzer<double>& ana,
                                        metadata_writer& mdata_writer,
                                        uint64_t ram_budget)
{
    std::mutex io_mutex;
    std::mutex corpus_mutex;
    std::mutex vocab_mutex;
    printing::progress progress{" > Tokenizing Docs: ", docs->size()};

    util::probe_set<std::string> vocab;
    bool exceeded_budget = false;
    auto task = [&](size_t chunk_id)
    {
        std::ofstream chunk{idx_->index_name() + "/chunk-"
                                + std::to_string(chunk_id),
                            std::ios::binary};
        auto analyzer = ana.clone();
        while (true)
        {
            util::optional<corpus::document> doc;
            {
                std::lock_guard<std::mutex> lock{corpus_mutex};

                if (!docs->has_next())
                    return;

                doc = docs->next();
            }
            {
                std::lock_guard<std::mutex> lock{io_mutex};
                progress(doc->id());
            }

            auto counts = analyzer->analyze(*doc);

            // warn if there is an empty document
            if (counts.empty())
            {
                std::lock_guard<std::mutex> lock{io_mutex};
                LOG(progress) << '\n' << ENDLG;
                LOG(warning) << "Empty document (id = " << doc->id()
                             << ") generated!" << ENDLG;
            }

            auto length = std::accumulate(
                counts.begin(), counts.end(), 0ul,
                [](uint64_t acc, const std::pair<std::string, double>& count)
                {
                    return acc + std::round(count.second);
                });

            mdata_writer.write(doc->id(), length, counts.size(), doc->mdata());
            idx_->impl_->set_label(doc->id(), doc->label());

            forward_index::postings_data_type::count_t pd_counts;
            pd_counts.reserve(counts.size());
            {
                std::lock_guard<std::mutex> lock{vocab_mutex};
                for (const auto& count : counts)
                {
                    auto it = vocab.find(count.first);
                    if (it == vocab.end())
                        it = vocab.insert(count.first);

                    pd_counts.emplace_back(term_id{it.index()}, count.second);
                }

                if (!exceeded_budget && vocab.bytes_used() > ram_budget)
                {
                    exceeded_budget = true;
                    std::lock_guard<std::mutex> io_lock{io_mutex};
                    LOG(progress) << '\n' << ENDLG;
                    LOG(warning)
                        << "Exceeding RAM budget; indexing cannot "
                           "proceed without exceeding specified RAM budget"
                        << ENDLG;
                }
            }

            forward_index::postings_data_type pdata{doc->id()};
            pdata.set_counts(std::move(pd_counts));
            pdata.write_packed(chunk);
        }
    };

    parallel::thread_pool pool;
    auto num_threads = pool.thread_ids().size();
    std::vector<std::future<void>> futures;
    futures.reserve(num_threads);
    for (size_t i = 0; i < num_threads; ++i)
        futures.emplace_back(pool.submit_task(std::bind(task, i)));

    for (auto& fut : futures)
        fut.get();

    progress.end();

    merge_chunks(num_threads, std::move(vocab));
}

void forward_index::impl::merge_chunks(size_t num_chunks,
                                       util::probe_set<std::string> vocab)
{
    auto keys = vocab.extract_keys();
    // vocab is now empty, but has enough space for the vocabulary

    {
        // we now create a new vocab with the keys in sorted order
        vocabulary_map_writer writer{idx_->index_name() + "/"
                                     + idx_->impl_->files[TERM_IDS_MAPPING]};
        auto sorted_keys = keys;
        std::sort(sorted_keys.begin(), sorted_keys.end());
        for (const auto& key : sorted_keys)
        {
            // in memory vocab
            vocab.insert(key);

            // on disk vocab
            writer.insert(key);
        }
    }

    // term_id in a chunk file corresponds to the index into the keys
    // vector, which we can then use the new vocab to map to an index
    postings_file_writer<forward_index::postings_data_type> writer{
        idx_->index_name() + "/" + idx_->impl_->files[POSTINGS], vocab.size()};

    using input_chunk = chunk_reader<forward_index::postings_data_type>;
    std::vector<input_chunk> chunks;
    chunks.reserve(num_chunks);
    for (size_t i = 0; i < num_chunks; ++i)
    {
        auto filename = idx_->index_name() + "/chunk-" + std::to_string(i);
        if (filesystem::file_exists(filename)
            && filesystem::file_size(filename) > 0)
            chunks.emplace_back(filename);
    }

    printing::progress progress{
        " > Merging postings: ",
        std::accumulate(chunks.begin(), chunks.end(), 0ul,
                        [](uint64_t acc, const input_chunk& chunk)
                        {
                            return acc + chunk.total_bytes();
                        })};

    uint64_t total_read
        = std::accumulate(chunks.begin(), chunks.end(), 0ul,
                          [](uint64_t acc, const input_chunk& chunk)
                          {
                              return acc + chunk.bytes_read();
                          });

    while (!chunks.empty())
    {
        progress(total_read);

        // find the lowest doc id
        auto min_chunk = std::min_element(chunks.begin(), chunks.end());

        // steal the postings and advance the chunk
        auto to_write = min_chunk->postings();
        auto before = min_chunk->bytes_read();
        ++*min_chunk;
        total_read += min_chunk->bytes_read() - before;

        // if there were no more postings, remove the chunk for the input
        if (!*min_chunk)
            chunks.erase(min_chunk);

        // renumber the postings
        forward_index::postings_data_type::count_t counts;
        counts.reserve(to_write.counts().size());
        for (const auto& count : to_write.counts())
        {
            const auto& key = keys.at(count.first);
            auto it = vocab.find(key);
            assert(it != vocab.end());
            counts.emplace_back(term_id{it.index()}, count.second);
        }

        // set the new counts and write to the postings file
        to_write.set_counts(std::move(counts));
        writer.write(to_write);
    }
}

void forward_index::impl::create_libsvm_postings(const cpptoml::table& config)
{
    auto filename = idx_->index_name() + idx_->impl_->files[POSTINGS];

    auto docs = corpus::make_corpus(config);
    auto num_docs = docs->size();
    idx_->impl_->load_labels(num_docs);

    total_unique_terms_ = 0;
    {
        postings_file_writer<forward_index::postings_data_type> out{filename,
                                                                    num_docs};

        // make md_writer with empty schema
        metadata_writer md_writer{idx_->index_name(), num_docs, docs->schema()};

        printing::progress progress{" > Creating postings from libsvm data: ",
                                    num_docs};
        while (docs->has_next())
        {
            auto doc = docs->next();
            progress(doc.id());


            uint64_t num_unique = 0;
            double length = 0;
            forward_index::postings_data_type pdata{doc.id()};

            auto counts = io::libsvm_parser::counts(doc.content());
            for (const auto& count : counts)
            {
                ++num_unique;
                if (count.first > total_unique_terms_)
                    total_unique_terms_ = count.first;
                length += count.second;
            }

            pdata.set_counts(std::move(counts));
            out.write(pdata);

            md_writer.write(doc.id(), static_cast<uint64_t>(length), num_unique,
                            doc.mdata());
            idx_->impl_->set_label(doc.id(), doc.label());
        }

        // +1 since we subtracted one from each of the ids in the
        // libsvm_parser::counts() function
        ++total_unique_terms_;
    }

    // reload the label file to ensure it was flushed
    idx_->impl_->load_labels();

    LOG(info) << "Created compressed postings file ("
              << printing::bytes_to_units(filesystem::file_size(filename))
              << ")" << ENDLG;
}

void forward_index::impl::create_uninverted_metadata(const std::string& name)
{
    auto files = {DOC_LABELS,       LABEL_IDS_MAPPING,
                  TERM_IDS_MAPPING, TERM_IDS_MAPPING_INVERSE,
                  METADATA_DB,      METADATA_INDEX};

    for (const auto& file : files)
        filesystem::copy_file(name + idx_->impl_->files[file],
                              idx_->index_name() + idx_->impl_->files[file]);
}

bool forward_index::impl::is_libsvm_format(const cpptoml::table& config) const
{
    auto prefix = config.get_as<std::string>("prefix");
    auto dset = config.get_as<std::string>("dataset");
    auto corp = config.get_as<std::string>("corpus");

    if (!prefix || !dset || !corp)
        throw forward_index_exception{"failed to determine corpus type"};

    auto corp_filename = *prefix + "/" + *dset + "/" + *corp;
    if (!filesystem::file_exists(corp_filename))
    {
        throw forward_index_exception{"corpus configuration file ("
                                      + corp_filename + ") not present"};
    }

    auto corpus_config = cpptoml::parse_file(corp_filename);
    auto type = corpus_config->get_as<std::string>("type");

    if (!type)
    {
        throw forward_index_exception{
            "'type' key not present in corpus configuration file "
            + corp_filename};
    }

    auto analyzers = config.get_table_array("analyzers")->get();
    if (analyzers.size() != 1)
        return false;

    auto method = analyzers[0]->get_as<std::string>("method");
    if (!method)
        throw forward_index_exception{"failed to find analyzer method"};

    if (*method == "libsvm" && *type == corpus::libsvm_corpus::id)
        return true;

    if (*method == "libsvm" || *type == corpus::libsvm_corpus::id)
        throw forward_index_exception{"both analyzer and corpus type must be "
                                      "libsvm in order to use libsvm formatted "
                                      "data"};

    return false;
}

uint64_t forward_index::unique_terms() const
{
    return fwd_impl_->total_unique_terms_;
}

auto forward_index::search_primary(doc_id d_id) const
    -> std::shared_ptr<postings_data_type>
{
    return fwd_impl_->postings_->find(d_id);
}

util::optional<postings_stream<term_id, double>>
forward_index::stream_for(doc_id d_id) const
{
    return fwd_impl_->postings_->find_stream(d_id);
}

void forward_index::impl::uninvert(const inverted_index& inv_idx,
                                   uint64_t ram_budget)
{
    postings_inverter<forward_index> handler{idx_->index_name()};
    {
        auto producer = handler.make_producer(ram_budget);
        for (term_id t_id{0}; t_id < inv_idx.unique_terms(); ++t_id)
        {
            auto pdata = inv_idx.search_primary(t_id);
            producer(pdata->primary_key(), pdata->counts());
        }
    }

    handler.merge_chunks();
    compress(idx_->index_name() + idx_->impl_->files[POSTINGS],
             inv_idx.num_docs());
}

void forward_index::impl::compress(const std::string& filename,
                                   uint64_t num_docs)
{
    auto ucfilename = filename + ".uncompressed";
    filesystem::rename_file(filename, ucfilename);

    // create a scope to ensure the reader and writer close properly so we
    // can calculate the size of the compressed file and delete the
    // uncompressed version at the end
    {
        postings_file_writer<forward_index::postings_data_type> out{filename,
                                                                    num_docs};

        forward_index::index_pdata_type pdata;
        auto length = filesystem::file_size(ucfilename);

        std::ifstream in{ucfilename, std::ios::binary};
        uint64_t byte_pos = 0;

        printing::progress progress{
            " > Compressing postings: ", length, 500, 1024 /* 1KB */
        };
        // note: we will be accessing pdata in sorted order, but not every
        // doc_id is guaranteed to exist, so we must be mindful of document
        // gaps
        doc_id last_id{0};
        while (auto bytes = pdata.read_packed(in))
        {
            byte_pos += bytes;
            progress(byte_pos);

            // write out any gaps
            for (doc_id d_id{last_id + 1}; d_id < pdata.primary_key(); ++d_id)
            {
                forward_index::postings_data_type pd{d_id};
                out.write(pd);
            }

            // convert from int to double for feature values
            forward_index::postings_data_type::count_t counts;
            counts.reserve(pdata.counts().size());
            for (const auto& count : pdata.counts())
                counts.emplace_back(count.first, count.second);

            forward_index::postings_data_type to_write{pdata.primary_key()};
            to_write.set_counts(std::move(counts));
            out.write(to_write);

            last_id = pdata.primary_key();
        }
    }

    LOG(info) << "Created compressed postings file ("
              << printing::bytes_to_units(filesystem::file_size(filename))
              << ")" << ENDLG;

    filesystem::delete_file(ucfilename);
}

void forward_index::impl::load_postings()
{
    postings_ = {idx_->index_name() + idx_->impl_->files[POSTINGS]};
}
}
}
