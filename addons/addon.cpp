#include <napi.h>
#include <string>
#include <vector>
#include <cstdlib>

#include "db.h"

class IdyDbWrap : public Napi::ObjectWrap<IdyDbWrap> {
public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports) {
    Napi::Function fn = DefineClass(env, "IdyDb", {
      InstanceMethod("open", &IdyDbWrap::Open),
      InstanceMethod("close", &IdyDbWrap::Close),
      InstanceMethod("columnNextRow", &IdyDbWrap::ColumnNextRow),

      InstanceMethod("ragUpsertText", &IdyDbWrap::RagUpsertText),
      InstanceMethod("ragQueryContext", &IdyDbWrap::RagQueryContext),

      InstanceMethod("insertConstChar", &IdyDbWrap::InsertConstChar),
      InstanceMethod("insertBool", &IdyDbWrap::InsertBool),
      InstanceMethod("insertInt", &IdyDbWrap::InsertInt),

      InstanceMethod("setRowsIncluded", &IdyDbWrap::SetRowsIncluded),
      InstanceMethod("ragQueryContextIncludedOnly", &IdyDbWrap::RagQueryContextIncludedOnly),

      // NEW: structured hits (top-k + metadata)
      InstanceMethod("ragQueryHitsIncludedOnly", &IdyDbWrap::RagQueryHitsIncludedOnly),

      InstanceMethod("deleteCell", &IdyDbWrap::DeleteCell),
    });

    exports.Set("IdyDb", fn);
    return exports;
  }

  IdyDbWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<IdyDbWrap>(info) {}

private:
  idydb* db_ = nullptr;
  
  bool IsOk(int rc) {
    return rc == IDYDB_DONE || rc == IDYDB_SUCCESS;
  }

  void ThrowDbError(const Napi::Env& env, int rc, const char* where = "") {
    const char* msg = "(no handler)";
    if (db_) {
      const char* raw = idydb_errmsg(&db_);
      // Never allow empty error strings to bubble up to JS.
      if (raw && *raw) msg = raw;
      else msg = "(no error detail; err_message was empty)";
    }
    std::string w = (where && *where) ? (std::string(where) + ": ") : "";
    Napi::Error::New(env, w + "IdyDB error rc=" + std::to_string(rc) + " msg=" + msg)
      .ThrowAsJavaScriptException();
  }

  bool EnsureOpen(const Napi::Env& env) {
    if (!db_) {
      Napi::Error::New(env, "IdyDb is not open").ThrowAsJavaScriptException();
      return false;
    }
    return true;
  }

  Napi::Value Open(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    std::string path = info[0].As<Napi::String>();
    int flags = info[1].As<Napi::Number>().Int32Value();

    int rc = idydb_open(path.c_str(), &db_, flags);
    if (rc != IDYDB_SUCCESS) {
      ThrowDbError(env, rc, "Open(...)");
      return env.Null();
    }
    return env.Undefined();
  }

  Napi::Value Close(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (db_) {
      idydb_close(&db_);
      db_ = nullptr;
    }
    return env.Undefined();
  }

  Napi::Value ColumnNextRow(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (!EnsureOpen(env)) return env.Null();
    auto col = (idydb_column_row_sizing)info[0].As<Napi::Number>().Int64Value();
    idydb_column_row_sizing r = idydb_column_next_row(&db_, col);
    return Napi::Number::New(env, (double)r);
  }

  Napi::Value DeleteCell(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (!EnsureOpen(env)) return env.Null();
    auto col = (idydb_column_row_sizing)info[0].As<Napi::Number>().Int64Value();
    auto row = (idydb_column_row_sizing)info[1].As<Napi::Number>().Int64Value();
    int rc = idydb_delete(&db_, col, row);
    if (rc != IDYDB_DONE) {
      ThrowDbError(env, rc, "DeleteCell");
    }
    return env.Undefined();
  }

  Napi::Value RagUpsertText(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (!EnsureOpen(env)) return env.Null();
    auto textCol = (idydb_column_row_sizing)info[0].As<Napi::Number>().Int64Value();
    auto vecCol  = (idydb_column_row_sizing)info[1].As<Napi::Number>().Int64Value();
    auto row     = (idydb_column_row_sizing)info[2].As<Napi::Number>().Int64Value();
    std::string text = info[3].As<Napi::String>();

    Napi::Float32Array arr = info[4].As<Napi::Float32Array>();
    unsigned short dims = (unsigned short)arr.ElementLength();

    std::vector<float> v(dims);
    for (unsigned short i = 0; i < dims; ++i) v[i] = arr[i];

    int rc = idydb_rag_upsert_text(&db_, textCol, vecCol, row, text.c_str(), v.data(), dims);
    if (rc != IDYDB_DONE) {
      ThrowDbError(env, rc, "RagUpsertText");
    }
    return env.Undefined();
  }

  Napi::Value RagQueryContext(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (!EnsureOpen(env)) return env.Null();
    auto textCol = (idydb_column_row_sizing)info[0].As<Napi::Number>().Int64Value();
    auto vecCol  = (idydb_column_row_sizing)info[1].As<Napi::Number>().Int64Value();

    Napi::Float32Array qArr = info[2].As<Napi::Float32Array>();
    unsigned short dims = (unsigned short)qArr.ElementLength();

    unsigned short k = (unsigned short)info[3].As<Napi::Number>().Uint32Value();
    int metric = info[4].As<Napi::Number>().Int32Value();
    size_t maxChars = (size_t)info[5].As<Napi::Number>().Int64Value();

    std::vector<float> q(dims);
    for (unsigned short i = 0; i < dims; ++i) q[i] = qArr[i];

    char* out = nullptr;
    int rc = idydb_rag_query_context(&db_, textCol, vecCol, q.data(), dims, k, (idydb_similarity_metric)metric, maxChars, &out);
    if (rc != IDYDB_DONE) {
      if (out) idydb_free(out);
      ThrowDbError(env, rc, "RagQueryContext");
      return env.Null();
    }

    std::string s = out ? std::string(out) : std::string();
    if (out) idydb_free(out);
    return Napi::String::New(env, s);
  }

  // ---------------------------------------------------------------------------
  // metadata helpers
  // ---------------------------------------------------------------------------

  Napi::Value InsertConstChar(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (!EnsureOpen(env)) return env.Null();

    auto col = (idydb_column_row_sizing)info[0].As<Napi::Number>().Int64Value();
    auto row = (idydb_column_row_sizing)info[1].As<Napi::Number>().Int64Value();
    std::string s = info[2].As<Napi::String>();

    int rc = idydb_insert_const_char(&db_, col, row, s.c_str());
    if (rc != IDYDB_DONE) ThrowDbError(env, rc, "InsertConstChar");
    return env.Undefined();
  }

  Napi::Value InsertBool(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (!EnsureOpen(env)) return env.Null();

    auto col = (idydb_column_row_sizing)info[0].As<Napi::Number>().Int64Value();
    auto row = (idydb_column_row_sizing)info[1].As<Napi::Number>().Int64Value();
    bool v = info[2].As<Napi::Boolean>().Value();

    int rc = idydb_insert_bool(&db_, col, row, v);
    if (rc != IDYDB_DONE) ThrowDbError(env, rc, "InsertBool");
    return env.Undefined();
  }

  Napi::Value InsertInt(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (!EnsureOpen(env)) return env.Null();

    auto col = (idydb_column_row_sizing)info[0].As<Napi::Number>().Int64Value();
    auto row = (idydb_column_row_sizing)info[1].As<Napi::Number>().Int64Value();
    int v = info[2].As<Napi::Number>().Int32Value();

    int rc = idydb_insert_int(&db_, col, row, v);
    if (rc != IDYDB_DONE) ThrowDbError(env, rc, "InsertInt");
    return env.Undefined();
  }

  // JS: setRowsIncluded(includedCol, rowsArray, includedBool)
  Napi::Value SetRowsIncluded(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (!EnsureOpen(env)) return env.Null();

    auto includedCol = (idydb_column_row_sizing)info[0].As<Napi::Number>().Int64Value();
    Napi::Array rows = info[1].As<Napi::Array>();
    bool included = info[2].As<Napi::Boolean>().Value();

    const uint32_t n = rows.Length();
    for (uint32_t i = 0; i < n; i++) {
      Napi::Value v = rows.Get(i);
      if (!v.IsNumber()) continue;
      auto row = (idydb_column_row_sizing)v.As<Napi::Number>().Int64Value();
      if (row <= 0) continue;
      int rc = idydb_insert_bool(&db_, includedCol, row, included);
      if (rc != IDYDB_DONE) {
        ThrowDbError(env, rc, "SetRowsIncluded");
        return env.Null();
      }
    }
    return env.Undefined();
  }

  // JS: ragQueryContextIncludedOnly(textCol, vecCol, includedCol, queryVec, k, metric, maxChars)
  Napi::Value RagQueryContextIncludedOnly(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (!EnsureOpen(env)) return env.Null();

    auto textCol     = (idydb_column_row_sizing)info[0].As<Napi::Number>().Int64Value();
    auto vecCol      = (idydb_column_row_sizing)info[1].As<Napi::Number>().Int64Value();
    auto includedCol = (idydb_column_row_sizing)info[2].As<Napi::Number>().Int64Value();

    Napi::Float32Array qArr = info[3].As<Napi::Float32Array>();
    unsigned short dims = (unsigned short)qArr.ElementLength();

    unsigned short k = (unsigned short)info[4].As<Napi::Number>().Uint32Value();
    int metric = info[5].As<Napi::Number>().Int32Value();
    size_t maxChars = (size_t)info[6].As<Napi::Number>().Int64Value();

    std::vector<float> q(dims);
    for (unsigned short i = 0; i < dims; ++i) q[i] = qArr[i];

    // Build filter: INCLUDED_COL == true
    idydb_filter_term term;
    term.column = includedCol;
    term.type   = IDYDB_BOOL;
    term.op     = IDYDB_FILTER_OP_EQ;
    term.value.b = true;

    idydb_filter filter;
    filter.terms = &term;
    filter.nterms = 1;

    char* out = nullptr;
    int rc = idydb_rag_query_context_filtered(
      &db_,
      textCol,
      vecCol,
      q.data(),
      dims,
      k,
      (idydb_similarity_metric)metric,
      &filter,
      maxChars,
      &out
    );

    if (rc != IDYDB_DONE) {
      if (out) idydb_free(out);
      ThrowDbError(env, rc, "RagQueryContextIncludedOnly");
      return env.Null();
    }

    std::string s = out ? std::string(out) : std::string();
    if (out) idydb_free(out);
    return Napi::String::New(env, s);
  }

  // JS:
  // ragQueryHitsIncludedOnly(textCol, vecCol, includedCol, relCol, queryVec, k, metric, metaColsArray, relFilter?)
  Napi::Value RagQueryHitsIncludedOnly(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (!EnsureOpen(env)) return env.Null();

    auto textCol     = (idydb_column_row_sizing)info[0].As<Napi::Number>().Int64Value();
    auto vecCol      = (idydb_column_row_sizing)info[1].As<Napi::Number>().Int64Value();
    auto includedCol = (idydb_column_row_sizing)info[2].As<Napi::Number>().Int64Value();
    auto relCol      = (idydb_column_row_sizing)info[3].As<Napi::Number>().Int64Value();

    Napi::Float32Array qArr = info[4].As<Napi::Float32Array>();
    unsigned short dims = (unsigned short)qArr.ElementLength();

    unsigned short k = (unsigned short)info[5].As<Napi::Number>().Uint32Value();
    int metric = info[6].As<Napi::Number>().Int32Value();

    Napi::Array metaColsJs = info[7].As<Napi::Array>();
    std::vector<idydb_column_row_sizing> metaCols;
    metaCols.reserve(metaColsJs.Length());
    for (uint32_t i = 0; i < metaColsJs.Length(); i++) {
      Napi::Value v = metaColsJs.Get(i);
      if (!v.IsNumber()) continue;
      metaCols.push_back((idydb_column_row_sizing)v.As<Napi::Number>().Int64Value());
    }

    std::string relFilter;
    bool hasRelFilter = false;
    if (info.Length() >= 9 && info[8].IsString()) {
      relFilter = info[8].As<Napi::String>().Utf8Value();
      if (!relFilter.empty()) hasRelFilter = true;
    }

    std::vector<float> q(dims);
    for (unsigned short i = 0; i < dims; ++i) q[i] = qArr[i];

    // Filter: INCLUDED == true [AND REL == relFilter]
    idydb_filter_term terms[2];
    size_t nterms = 0;

    terms[nterms].column = includedCol;
    terms[nterms].type   = IDYDB_BOOL;
    terms[nterms].op     = IDYDB_FILTER_OP_EQ;
    terms[nterms].value.b = true;
    nterms++;

    if (hasRelFilter) {
      terms[nterms].column = relCol;
      terms[nterms].type   = IDYDB_CHAR;
      terms[nterms].op     = IDYDB_FILTER_OP_EQ;
      terms[nterms].value.s = relFilter.c_str(); // valid for duration of this call
      nterms++;
    }

    idydb_filter filter;
    filter.terms = terms;
    filter.nterms = nterms;

    std::vector<idydb_knn_result> outResults(k);
    char** outTexts = (char**)std::calloc(k, sizeof(char*));
    if (!outTexts) {
      Napi::Error::New(env, "calloc failed").ThrowAsJavaScriptException();
      return env.Null();
    }

    const size_t metaCount = metaCols.size();
    std::vector<idydb_value> outMeta;
    if (metaCount > 0) outMeta.resize((size_t)k * metaCount);

    int rc = idydb_rag_query_topk_with_metadata(
      &db_,
      textCol,
      vecCol,
      q.data(),
      dims,
      k,
      (idydb_similarity_metric)metric,
      &filter,
      metaCount ? metaCols.data() : nullptr,
      metaCount,
      outResults.data(),
      outTexts,
      metaCount ? outMeta.data() : nullptr
    );

    // NOTE: idydb_rag_query_topk_with_metadata returns:
    //   n (0..k) on success, -1 on error.
    if (rc < 0) {
      for (unsigned short i = 0; i < k; i++) {
        if (outTexts[i]) idydb_free(outTexts[i]);
      }
      std::free(outTexts);
      if (metaCount) idydb_values_free(outMeta.data(), (size_t)k * metaCount);

      // error details are already stored in db_->err_message
      ThrowDbError(env, rc, "RagQueryHitsIncludedOnly");
      return env.Null();
    }

    const unsigned short n = (unsigned short)rc;

    // Build JS array (iterate only n results)
    Napi::Array arr = Napi::Array::New(env);
    uint32_t outN = 0;

    for (unsigned short i = 0; i < n; i++) {
      const auto row = outResults[i].row;
      if ((unsigned long long)row == 0ULL) {
        if (outTexts[i]) idydb_free(outTexts[i]);
        continue;
      }

      Napi::Object hit = Napi::Object::New(env);
      hit.Set("row", Napi::Number::New(env, (double)row));
      hit.Set("score", Napi::Number::New(env, (double)outResults[i].score));

      std::string txt = outTexts[i] ? std::string(outTexts[i]) : std::string();
      if (outTexts[i]) idydb_free(outTexts[i]);
      hit.Set("text", Napi::String::New(env, txt));

      // meta: { "colNumber": value, ... }
      Napi::Object meta = Napi::Object::New(env);
      for (size_t j = 0; j < metaCount; j++) {
        const idydb_value& v = outMeta[(size_t)i * metaCount + j];
        const std::string key = std::to_string((unsigned long long)metaCols[j]);

        switch (v.type) {
          case IDYDB_NULL:
            meta.Set(key, env.Null());
            break;
          case IDYDB_INTEGER:
            meta.Set(key, Napi::Number::New(env, (double)v.as.i));
            break;
          case IDYDB_FLOAT:
            meta.Set(key, Napi::Number::New(env, (double)v.as.f));
            break;
          case IDYDB_BOOL:
            meta.Set(key, Napi::Boolean::New(env, v.as.b));
            break;
          case IDYDB_CHAR:
            meta.Set(key, Napi::String::New(env, v.as.s ? std::string(v.as.s) : std::string()));
            break;
          default:
            meta.Set(key, env.Null());
            break;
        }
      }
      hit.Set("meta", meta);

      arr.Set(outN++, hit);
    }

    // Free any remaining outTexts not consumed by the loop
    for (unsigned short i = n; i < k; i++) {
      if (outTexts[i]) idydb_free(outTexts[i]);
    }

    std::free(outTexts);
    if (metaCount) idydb_values_free(outMeta.data(), (size_t)k * metaCount);

    return arr;
  }
};

Napi::Object InitAll(Napi::Env env, Napi::Object exports) {
  return IdyDbWrap::Init(env, exports);
}

NODE_API_MODULE(idydb, InitAll)
