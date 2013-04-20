/**
 * @file ngram_lex_tokenizer.cpp
 */

#include <unordered_map>
#include "io/parser.h"
#include "tokenizers/ngram_lex_tokenizer.h"

namespace meta {
namespace tokenizers {

ngram_lex_tokenizer::ngram_lex_tokenizer(size_t n):
    ngram_simple_tokenizer(n) { /* nothing */ }

void ngram_lex_tokenizer::tokenize_document(index::Document & document,
        std::function<term_id(const std::string &)> mapping,
        const std::shared_ptr<std::unordered_map<term_id, unsigned int>> & docFreq)
{
    io::Parser parser(document.getPath() + ".lex", " \n");
    simple_tokenize(parser, document, mapping, docFreq);
}

}
}
