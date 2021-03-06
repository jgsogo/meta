/**
 * @file classifier_test.cpp
 * @author Sean Massung
 */

#include <random>

#include "test/classifier_test.h"
#include "classify/loss/all.h"

namespace meta
{
namespace testing
{

template <class Index, class Classifier>
void check_cv(Index& idx, Classifier& c, double min_accuracy)
{
    std::vector<doc_id> docs = idx.docs();
    classify::confusion_matrix mtx = c.cross_validate(docs, 5);
    ASSERT_GREATER(mtx.accuracy(), min_accuracy);
    ASSERT_LESS(mtx.accuracy(), 100.0);
}

template <class Index, class Classifier>
void check_split(Index& idx, Classifier& c, double min_accuracy)
{
    // create splits
    std::vector<doc_id> docs = idx.docs();
    std::mt19937 gen(47);
    std::shuffle(docs.begin(), docs.end(), gen);
    size_t split_idx = docs.size() / 8;
    std::vector<doc_id> train_docs{docs.begin() + split_idx, docs.end()};
    std::vector<doc_id> test_docs{docs.begin(), docs.begin() + split_idx};

    // train and test
    c.train(train_docs);
    classify::confusion_matrix mtx = c.test(test_docs);
    ASSERT_GREATER(mtx.accuracy(), min_accuracy);
    ASSERT_LESS(mtx.accuracy(), 100.0);
}

int run_tests(const std::string& type)
{
    using namespace classify;

    int num_failed = 0;

    // scope to ensure that the index objects are destroyed before trying
    // to delete their directory; this is needed for weirdness on NFS or
    // other filesystems that might lock opened files
    {
        auto i_idx
            = index::make_index<index::inverted_index, caching::no_evict_cache>(
                "test-config.toml");
        auto f_idx
            = index::make_index<index::forward_index, caching::no_evict_cache>(
                "test-config.toml");

        num_failed += testing::run_test("naive-bayes-cv-" + type, [&]()
                                        {
                                            naive_bayes nb{f_idx};
                                            check_cv(*f_idx, nb, 0.84);
                                        });

        num_failed += testing::run_test("naive-bayes-split-" + type, [&]()
                                        {
                                            naive_bayes nb{f_idx};
                                            check_split(*f_idx, nb, 0.83);
                                        });

        num_failed += testing::run_test(
            "knn-cv-" + type, [&]()
            {
                knn kn{i_idx, f_idx, 10, make_unique<index::okapi_bm25>()};
                check_cv(*f_idx, kn, 0.90);
            });

        num_failed += testing::run_test(
            "knn-split-" + type, [&]()
            {
                knn kn{i_idx, f_idx, 10, make_unique<index::okapi_bm25>()};
                check_split(*f_idx, kn, 0.88);
            });

        num_failed += testing::run_test("nearest-centroid-cv-" + type, [&]()
                                        {
                                            nearest_centroid nc{i_idx, f_idx};
                                            check_cv(*f_idx, nc, 0.88);
                                        });

        num_failed += testing::run_test("nearest-centroid-split-" + type, [&]()
                                        {
                                            nearest_centroid nc{i_idx, f_idx};
                                            check_split(*f_idx, nc, 0.84);
                                        });

        num_failed += testing::run_test(
            "sgd-cv-" + type, [&]()
            {
                one_vs_all hinge_sgd{f_idx, [&](class_label positive)
                                     {
                                         return make_unique<sgd>(
                                             "sgd-model-test", f_idx, positive,
                                             class_label{"negative"},
                                             make_unique<loss::hinge>());
                                     }};
                check_cv(*f_idx, hinge_sgd, 0.93);
                one_vs_all perceptron{f_idx, [&](class_label positive)
                                      {
                                          return make_unique<sgd>(
                                              "sgd-model-test", f_idx, positive,
                                              class_label{"negative"},
                                              make_unique<loss::perceptron>());
                                      }};
                check_cv(*f_idx, perceptron, 0.89);
            });

        num_failed += testing::run_test(
            "sgd-split-" + type, [&]()
            {
                one_vs_all hinge_sgd{f_idx, [&](class_label positive)
                                     {
                                         return make_unique<sgd>(
                                             "sgd-model-test", f_idx, positive,
                                             class_label{"negative"},
                                             make_unique<loss::hinge>());
                                     }};
                check_split(*f_idx, hinge_sgd, 0.89);
                one_vs_all perceptron{f_idx, [&](class_label positive)
                                      {
                                          return make_unique<sgd>(
                                              "sgd-model-test", f_idx, positive,
                                              class_label{"negative"},
                                              make_unique<loss::perceptron>());
                                      }};
                check_split(*f_idx, perceptron, 0.85);
            });

        num_failed += testing::run_test(
            "log-reg-cv-" + type, [&]()
            {
                logistic_regression logreg{"logreg-model-test", f_idx};
                check_cv(*f_idx, logreg, 0.90);
            });

        num_failed += testing::run_test(
            "log-reg-split-" + type, [&]()
            {
                logistic_regression logreg{"logreg-model-test", f_idx};
                check_split(*f_idx, logreg, 0.87);
            });

        num_failed += testing::run_test("winnow-cv-" + type, [&]()
                                        {
                                            winnow win{f_idx};
                                            check_cv(*f_idx, win, 0.80);
                                        });

        num_failed += testing::run_test("winnow-split-" + type, [&]()
                                        {
                                            winnow win{f_idx};
                                            // this is *really* low... is winnow
                                            // broken?
                                            check_split(*f_idx, win, 0.65);
                                        });

        num_failed += testing::run_test(
            "svm-wrapper-" + type, [&]()
            {
                auto config = cpptoml::parse_file("test-config.toml");
                auto mod_path = config.get_as<std::string>("libsvm-modules");
                if (!mod_path)
                    throw std::runtime_error{"no path for libsvm-modules"};
                svm_wrapper svm{f_idx, *mod_path};
                check_cv(*f_idx, svm, .80);
            });
    }

    system("rm -rf ceeaus-*");
    return num_failed;
}

int confusion_matrix_test()
{
    return testing::run_test(
        "matrix-test", [&]()
        {
            // We have 3 classes {A, B, C} and get the following predictions:
            std::vector<std::pair<predicted_label, class_label>> preds;
            preds.emplace_back("A"_pl, "A"_cl);
            preds.emplace_back("B"_pl, "A"_cl);
            preds.emplace_back("C"_pl, "A"_cl);
            preds.emplace_back("B"_pl, "B"_cl);
            preds.emplace_back("B"_pl, "B"_cl);
            preds.emplace_back("B"_pl, "B"_cl);
            preds.emplace_back("A"_pl, "C"_cl);
            preds.emplace_back("A"_pl, "C"_cl);
            preds.emplace_back("A"_pl, "C"_cl);

            classify::confusion_matrix mtx;
            for (auto& pair : preds)
                mtx.add(pair.first, pair.second);

            ASSERT_APPROX_EQUAL(mtx.accuracy(), 4.0 / 9);

            ASSERT_APPROX_EQUAL(mtx.precision("A"_cl), 1.0 / 4);
            ASSERT_APPROX_EQUAL(mtx.precision("B"_cl), 3.0 / 4);
            ASSERT_APPROX_EQUAL(mtx.precision("C"_cl), 0.0);
            ASSERT_APPROX_EQUAL(mtx.precision(), 1.0 / 3);

            ASSERT_APPROX_EQUAL(mtx.recall("A"_cl), 1.0 / 3);
            ASSERT_APPROX_EQUAL(mtx.recall("B"_cl), 1.0);
            ASSERT_APPROX_EQUAL(mtx.recall("C"_cl), 0.0);
            ASSERT_APPROX_EQUAL(mtx.recall(), 4.0 / 9);

            ASSERT_APPROX_EQUAL(mtx.f1_score("A"_cl), 2.0 / 7);
            ASSERT_APPROX_EQUAL(mtx.f1_score("B"_cl), 6.0 / 7);
            ASSERT_APPROX_EQUAL(mtx.f1_score("C"_cl), 0.0);
            ASSERT_APPROX_EQUAL(mtx.f1_score(), (2.0 / 7 + 6.0 / 7) / 3);
        });
}

int classifier_tests()
{
    int num_failed = 0;
    system("rm -rf ceeaus-*");
    create_config("file");
    num_failed += run_tests("file");
    create_config("line");
    num_failed += run_tests("line");
    num_failed += confusion_matrix_test();
    return num_failed;
}
}
}
