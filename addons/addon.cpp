#include <napi.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "db.h"

#ifdef IDYDB_TESTING
#include <atomic>
#include <cerrno>
#include "../idydb/tests/failure_hooks.h"
#endif

namespace {
constexpr double kMaxSafeInteger = 9007199254740991.0;

void RequireCount(const Napi::CallbackInfo& info, size_t minimum, size_t maximum) {
  if (info.Length() < minimum || info.Length() > maximum) {
    throw Napi::TypeError::New(info.Env(), "Unexpected number of arguments");
  }
}

#ifdef IDYDB_TESTING
// Keep one-shot fault state native: the finalizer must never call JavaScript.
std::atomic<bool> testFailNextSync{false};
std::atomic<uint32_t> testSyncFailures{0};

int TestSyncHook(int stage, void*) {
  if (stage == IDYDB_TEST_FILE_SYNC && testFailNextSync.exchange(false)) {
    testSyncFailures.fetch_add(1);
    return EIO;
  }
  return 0;
}

Napi::Value TestFailNextSync(const Napi::CallbackInfo& info) {
  RequireCount(info, 0, 0);
  if (testFailNextSync.exchange(true)) {
    throw Napi::Error::New(info.Env(), "A test sync failure is already pending");
  }
  idydb_test_set_hook(TestSyncHook, nullptr);
  return info.Env().Undefined();
}

Napi::Value TestSyncFailures(const Napi::CallbackInfo& info) {
  RequireCount(info, 0, 0);
  return Napi::Number::New(info.Env(), testSyncFailures.load());
}
#endif

double Integer(const Napi::Value& value, const char* name, double minimum, double maximum) {
  if (!value.IsNumber()) {
    throw Napi::TypeError::New(value.Env(), std::string(name) + " must be a number");
  }
  const double number = value.As<Napi::Number>().DoubleValue();
  if (!std::isfinite(number) || std::floor(number) != number || number < minimum || number > maximum) {
    throw Napi::RangeError::New(value.Env(), std::string(name) + " must be an integer within the supported range");
  }
  return number;
}

idydb_column_row_sizing Position(const Napi::Value& value, const char* name) {
  return static_cast<idydb_column_row_sizing>(Integer(value, name, 1, IDYDB_MAX_POSITION));
}

bool Boolean(const Napi::Value& value, const char* name) {
  if (!value.IsBoolean()) {
    throw Napi::TypeError::New(value.Env(), std::string(name) + " must be a boolean");
  }
  return value.As<Napi::Boolean>().Value();
}

std::string String(const Napi::Value& value, const char* name, bool text = true) {
  if (!value.IsString()) {
    throw Napi::TypeError::New(value.Env(), std::string(name) + " must be a string");
  }
  std::string result = value.As<Napi::String>().Utf8Value();
  if (result.find('\0') != std::string::npos || (text && result.size() > IDYDB_MAX_TEXT_BYTES)) {
    throw Napi::RangeError::New(value.Env(), std::string(name) + " contains NUL or exceeds the byte limit");
  }
  return result;
}

std::vector<float> Vector(const Napi::Value& value) {
  if (!value.IsTypedArray() || value.As<Napi::TypedArray>().TypedArrayType() != napi_float32_array) {
    throw Napi::TypeError::New(value.Env(), "Embedding must be a Float32Array");
  }
  const Napi::Float32Array input = value.As<Napi::Float32Array>();
  const size_t length = input.ElementLength();
  if (length == 0 || length > IDYDB_MAX_VECTOR_DIMENSIONS) {
    throw Napi::RangeError::New(value.Env(), "Embedding dimensions must be in [1,16383]");
  }
  std::vector<float> result(length);
  for (size_t i = 0; i < length; ++i) {
    result[i] = input[i];
    if (!std::isfinite(result[i])) throw Napi::RangeError::New(value.Env(), "Embedding elements must be finite");
  }
  return result;
}

unsigned short TopK(const Napi::Value& value) {
  return static_cast<unsigned short>(Integer(value, "k", 1, 65535));
}

idydb_similarity_metric Metric(const Napi::Value& value) {
  return static_cast<idydb_similarity_metric>(Integer(value, "metric", IDYDB_SIM_COSINE, IDYDB_SIM_L2));
}

size_t MaxChars(const Napi::Value& value) {
  const double maximum = std::min(kMaxSafeInteger, static_cast<double>(std::numeric_limits<size_t>::max() - 1));
  return static_cast<size_t>(Integer(value, "maxChars", 0, maximum));
}

std::vector<idydb_column_row_sizing> Positions(const Napi::Value& value, const char* name) {
  if (!value.IsArray()) throw Napi::TypeError::New(value.Env(), std::string(name) + " must be an array");
  const Napi::Array array = value.As<Napi::Array>();
  std::vector<idydb_column_row_sizing> result;
  result.reserve(array.Length());
  for (uint32_t i = 0; i < array.Length(); ++i) result.push_back(Position(array.Get(i), name));
  return result;
}

struct DbFree {
  void operator()(char* value) const { idydb_free(value); }
};

// Own native outputs throughout JS conversion, including exceptions thrown by
// allocations or property setters. Every pointer is freed exactly once.
struct QueryOutput {
  std::vector<idydb_knn_result> results;
  std::vector<char*> texts;
  std::vector<idydb_value> metadata;

  QueryOutput(size_t k, size_t columns) : results(k), texts(k, nullptr) {
    if (columns > metadata.max_size() / k) throw std::length_error("Metadata output is too large");
    metadata.resize(k * columns);
    for (auto& value : metadata) value.type = IDYDB_NULL;
  }
  ~QueryOutput() {
    for (char* text : texts) idydb_free(text);
    idydb_values_free(metadata.data(), metadata.size());
  }
};
} // namespace

class IdyDbWrap : public Napi::ObjectWrap<IdyDbWrap> {
public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports) {
    const Napi::Function constructor = DefineClass(env, "IdyDb", {
      InstanceMethod("open", &IdyDbWrap::Open),
      InstanceMethod("flush", &IdyDbWrap::Flush),
      InstanceMethod("close", &IdyDbWrap::Close),
      InstanceMethod("discard", &IdyDbWrap::Discard),
      InstanceMethod("columnNextRow", &IdyDbWrap::ColumnNextRow),
      InstanceMethod("ragUpsertText", &IdyDbWrap::RagUpsertText),
      InstanceMethod("ragQueryContext", &IdyDbWrap::RagQueryContext),
      InstanceMethod("insertConstChar", &IdyDbWrap::InsertConstChar),
      InstanceMethod("insertBool", &IdyDbWrap::InsertBool),
      InstanceMethod("insertInt", &IdyDbWrap::InsertInt),
      InstanceMethod("setRowsIncluded", &IdyDbWrap::SetRowsIncluded),
      InstanceMethod("ragQueryContextIncludedOnly", &IdyDbWrap::RagQueryContextIncludedOnly),
      InstanceMethod("ragQueryHitsIncludedOnly", &IdyDbWrap::RagQueryHitsIncludedOnly),
      InstanceMethod("deleteCell", &IdyDbWrap::DeleteCell),
    });
    exports.Set("IdyDb", constructor);
    exports.Set("IDYDB_CREATE", IDYDB_CREATE);
    exports.Set("IDYDB_READONLY", IDYDB_READONLY);
    exports.Set("IDYDB_UNSAFE", IDYDB_UNSAFE);
    exports.Set("IDYDB_SIM_COSINE", static_cast<int>(IDYDB_SIM_COSINE));
    exports.Set("IDYDB_SIM_L2", static_cast<int>(IDYDB_SIM_L2));
#ifdef IDYDB_TESTING
    exports.Set("__testFailNextSync", Napi::Function::New(env, TestFailNextSync));
    exports.Set("__testSyncFailures", Napi::Function::New(env, TestSyncFailures));
#endif
    return exports;
  }

  explicit IdyDbWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<IdyDbWrap>(info) {}
  ~IdyDbWrap() override {
    if (db_ && idydb_close(&db_) != IDYDB_DONE) idydb_discard(&db_);
  }

private:
  idydb* db_ = nullptr;

  Napi::Error DbError(Napi::Env env, int rc, const char* operation) {
    const char* detail = db_ ? idydb_errmsg(&db_) : nullptr;
    return Napi::Error::New(env, std::string(operation) + ": IdyDB error rc=" + std::to_string(rc) +
      " msg=" + (detail && *detail ? detail : "database operation failed"));
  }

  void EnsureOpen(Napi::Env env) {
    if (!db_) throw Napi::Error::New(env, "IdyDb is not open");
  }

  void Check(Napi::Env env, int rc, const char* operation) {
    if (rc != IDYDB_DONE && rc != IDYDB_SUCCESS) throw DbError(env, rc, operation);
  }

  Napi::Value Open(const Napi::CallbackInfo& info) {
    RequireCount(info, 2, 2);
    if (db_) throw Napi::Error::New(info.Env(), "IdyDb is already open; close it before reopening");
    const std::string path = String(info[0], "path", false);
    if (path.empty()) throw Napi::RangeError::New(info.Env(), "path must not be empty");
    const int flags = static_cast<int>(Integer(info[1], "flags", 0, 31));
    if ((flags & IDYDB_CREATE) != 0 && (flags & IDYDB_CREATE) != IDYDB_CREATE) {
      throw Napi::RangeError::New(info.Env(), "flags must combine IDYDB_CREATE, IDYDB_READONLY and IDYDB_UNSAFE");
    }
    const int rc = idydb_open(path.c_str(), &db_, flags);
    if (rc != IDYDB_SUCCESS) {
      const Napi::Error error = DbError(info.Env(), rc, "open");
      idydb_discard(&db_);
      throw error;
    }
    return info.Env().Undefined();
  }

  Napi::Value Flush(const Napi::CallbackInfo& info) {
    RequireCount(info, 0, 0);
    EnsureOpen(info.Env());
    Check(info.Env(), idydb_flush(&db_), "flush (database persistence failed)");
    return info.Env().Undefined();
  }

  Napi::Value Close(const Napi::CallbackInfo& info) {
    RequireCount(info, 0, 0);
    if (db_) {
      const int rc = idydb_close(&db_);
      Check(info.Env(), rc, "close (database persistence failed)");
    }
    return info.Env().Undefined();
  }

  Napi::Value Discard(const Napi::CallbackInfo& info) {
    RequireCount(info, 0, 0);
    Check(info.Env(), idydb_discard(&db_), "discard");
    return info.Env().Undefined();
  }

  Napi::Value ColumnNextRow(const Napi::CallbackInfo& info) {
    RequireCount(info, 1, 1);
    EnsureOpen(info.Env());
    const auto column = Position(info[0], "column");
    const auto row = idydb_column_next_row(&db_, column);
    if (row == 0) throw DbError(info.Env(), IDYDB_RANGE, "columnNextRow");
    return Napi::Number::New(info.Env(), static_cast<double>(row));
  }

  Napi::Value DeleteCell(const Napi::CallbackInfo& info) {
    RequireCount(info, 2, 2);
    EnsureOpen(info.Env());
    const auto column = Position(info[0], "column");
    const auto row = Position(info[1], "row");
    Check(info.Env(), idydb_delete(&db_, column, row), "deleteCell");
    return info.Env().Undefined();
  }

  Napi::Value RagUpsertText(const Napi::CallbackInfo& info) {
    RequireCount(info, 5, 5);
    EnsureOpen(info.Env());
    const auto textColumn = Position(info[0], "textColumn");
    const auto vectorColumn = Position(info[1], "vectorColumn");
    const auto row = Position(info[2], "row");
    const std::string text = String(info[3], "text");
    const auto embedding = Vector(info[4]);
    if (textColumn == vectorColumn) throw Napi::RangeError::New(info.Env(), "Text and vector columns must differ");
    Check(info.Env(), idydb_rag_upsert_text(&db_, textColumn, vectorColumn, row, text.c_str(),
      embedding.data(), static_cast<unsigned short>(embedding.size())), "ragUpsertText");
    return info.Env().Undefined();
  }

  Napi::Value RagQueryContext(const Napi::CallbackInfo& info) {
    RequireCount(info, 6, 6);
    EnsureOpen(info.Env());
    const auto textColumn = Position(info[0], "textColumn");
    const auto vectorColumn = Position(info[1], "vectorColumn");
    const auto query = Vector(info[2]);
    const auto k = TopK(info[3]);
    const auto metric = Metric(info[4]);
    const auto maxChars = MaxChars(info[5]);
    char* raw = nullptr;
    const int rc = idydb_rag_query_context(&db_, textColumn, vectorColumn, query.data(),
      static_cast<unsigned short>(query.size()), k, metric, maxChars, &raw);
    const std::unique_ptr<char, DbFree> output(raw);
    Check(info.Env(), rc, "ragQueryContext");
    return Napi::String::New(info.Env(), output ? output.get() : "");
  }

  Napi::Value InsertConstChar(const Napi::CallbackInfo& info) {
    RequireCount(info, 3, 3);
    EnsureOpen(info.Env());
    const auto column = Position(info[0], "column");
    const auto row = Position(info[1], "row");
    const std::string value = String(info[2], "value");
    Check(info.Env(), idydb_insert_const_char(&db_, column, row, value.c_str()), "insertConstChar");
    return info.Env().Undefined();
  }

  Napi::Value InsertBool(const Napi::CallbackInfo& info) {
    RequireCount(info, 3, 3);
    EnsureOpen(info.Env());
    const auto column = Position(info[0], "column");
    const auto row = Position(info[1], "row");
    const bool value = Boolean(info[2], "value");
    Check(info.Env(), idydb_insert_bool(&db_, column, row, value), "insertBool");
    return info.Env().Undefined();
  }

  Napi::Value InsertInt(const Napi::CallbackInfo& info) {
    RequireCount(info, 3, 3);
    EnsureOpen(info.Env());
    const auto column = Position(info[0], "column");
    const auto row = Position(info[1], "row");
    const int value = static_cast<int>(Integer(info[2], "value", std::numeric_limits<int>::min(), std::numeric_limits<int>::max()));
    Check(info.Env(), idydb_insert_int(&db_, column, row, value), "insertInt");
    return info.Env().Undefined();
  }

  Napi::Value SetRowsIncluded(const Napi::CallbackInfo& info) {
    RequireCount(info, 3, 3);
    EnsureOpen(info.Env());
    const auto column = Position(info[0], "includedColumn");
    const auto rows = Positions(info[1], "rows");
    const bool included = Boolean(info[2], "included");
    // Validate the entire batch before changing any cells.
    for (const auto row : rows) Check(info.Env(), idydb_insert_bool(&db_, column, row, included), "setRowsIncluded");
    return info.Env().Undefined();
  }

  Napi::Value RagQueryContextIncludedOnly(const Napi::CallbackInfo& info) {
    RequireCount(info, 7, 7);
    EnsureOpen(info.Env());
    const auto textColumn = Position(info[0], "textColumn");
    const auto vectorColumn = Position(info[1], "vectorColumn");
    const auto includedColumn = Position(info[2], "includedColumn");
    const auto query = Vector(info[3]);
    const auto k = TopK(info[4]);
    const auto metric = Metric(info[5]);
    const auto maxChars = MaxChars(info[6]);
    idydb_filter_term term{};
    term.column = includedColumn;
    term.type = IDYDB_BOOL;
    term.op = IDYDB_FILTER_OP_EQ;
    term.value.b = true;
    const idydb_filter filter{&term, 1};
    char* raw = nullptr;
    const int rc = idydb_rag_query_context_filtered(&db_, textColumn, vectorColumn, query.data(),
      static_cast<unsigned short>(query.size()), k, metric, &filter, maxChars, &raw);
    const std::unique_ptr<char, DbFree> output(raw);
    Check(info.Env(), rc, "ragQueryContextIncludedOnly");
    return Napi::String::New(info.Env(), output ? output.get() : "");
  }

  Napi::Value RagQueryHitsIncludedOnly(const Napi::CallbackInfo& info) {
    RequireCount(info, 8, 9);
    EnsureOpen(info.Env());
    const auto textColumn = Position(info[0], "textColumn");
    const auto vectorColumn = Position(info[1], "vectorColumn");
    const auto includedColumn = Position(info[2], "includedColumn");
    const auto relationColumn = Position(info[3], "relationColumn");
    const auto query = Vector(info[4]);
    const auto k = TopK(info[5]);
    const auto metric = Metric(info[6]);
    const auto columns = Positions(info[7], "metadataColumns");
    const std::string relation = info.Length() == 9 && !info[8].IsUndefined() ? String(info[8], "relationFilter") : "";
    idydb_filter_term terms[2]{};
    terms[0].column = includedColumn;
    terms[0].type = IDYDB_BOOL;
    terms[0].op = IDYDB_FILTER_OP_EQ;
    terms[0].value.b = true;
    size_t termCount = 1;
    if (!relation.empty()) {
      terms[1].column = relationColumn;
      terms[1].type = IDYDB_CHAR;
      terms[1].op = IDYDB_FILTER_OP_EQ;
      terms[1].value.s = relation.c_str();
      termCount = 2;
    }
    const idydb_filter filter{terms, termCount};
    QueryOutput output(k, columns.size());
    const int count = idydb_rag_query_topk_with_metadata(&db_, textColumn, vectorColumn, query.data(),
      static_cast<unsigned short>(query.size()), k, metric, &filter, columns.data(), columns.size(),
      output.results.data(), output.texts.data(), output.metadata.data());
    if (count < 0) throw DbError(info.Env(), count, "ragQueryHitsIncludedOnly");
    if (count > k) throw Napi::Error::New(info.Env(), "Database returned more hits than requested");
    const Napi::Array hits = Napi::Array::New(info.Env(), static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
      const Napi::Object hit = Napi::Object::New(info.Env());
      hit.Set("row", Napi::Number::New(info.Env(), static_cast<double>(output.results[i].row)));
      hit.Set("score", Napi::Number::New(info.Env(), output.results[i].score));
      hit.Set("text", Napi::String::New(info.Env(), output.texts[i] ? output.texts[i] : ""));
      const Napi::Object metadata = Napi::Object::New(info.Env());
      for (size_t j = 0; j < columns.size(); ++j) {
        const auto& value = output.metadata[static_cast<size_t>(i) * columns.size() + j];
        const std::string key = std::to_string(columns[j]);
        switch (value.type) {
          case IDYDB_INTEGER: metadata.Set(key, Napi::Number::New(info.Env(), value.as.i)); break;
          case IDYDB_FLOAT: metadata.Set(key, Napi::Number::New(info.Env(), value.as.f)); break;
          case IDYDB_BOOL: metadata.Set(key, Napi::Boolean::New(info.Env(), value.as.b)); break;
          case IDYDB_CHAR: metadata.Set(key, Napi::String::New(info.Env(), value.as.s ? value.as.s : "")); break;
          case IDYDB_VECTOR: {
            Napi::Float32Array vector = Napi::Float32Array::New(info.Env(), value.as.vec.dims);
            for (size_t d = 0; d < value.as.vec.dims; ++d) vector[d] = value.as.vec.v[d];
            metadata.Set(key, vector);
            break;
          }
          default: metadata.Set(key, info.Env().Null()); break;
        }
      }
      hit.Set("meta", metadata);
      hits.Set(static_cast<uint32_t>(i), hit);
    }
    return hits;
  }
};

Napi::Object InitAll(Napi::Env env, Napi::Object exports) { return IdyDbWrap::Init(env, exports); }
NODE_API_MODULE(idydb, InitAll)
