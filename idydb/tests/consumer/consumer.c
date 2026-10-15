#include <db.h>
#include <stdio.h>
#include <string.h>

static const char callback_text[] = "installed C allocator callback";
struct callback_context {
    int marker;
    unsigned calls;
};

static int embed_from_consumer(const char* text, float** output,
                              unsigned short* dimensions, void* user) {
    struct callback_context* context = user;
    if (!context || context->marker != 73 || !text || strcmp(text, callback_text) != 0)
        return 1;
    ++context->calls;
    *output = idydb_alloc(2 * sizeof(float));
    if (!*output) return 1;
    (*output)[0] = 0.25f;
    (*output)[1] = -0.75f;
    *dimensions = 2;
    /* Ownership crosses back into the library, which releases this vector. */
    return 0;
}

int main(void) {
    const char* path = "installed-consumer-c.db";
    idydb* handle = NULL;
    int result = 1;
    struct callback_context context = {73, 0};
    unsigned short dimensions = 0;
    const float* vector = NULL;
    remove(path);
    if (idydb_open_encrypted(path, &handle, IDYDB_CREATE, "consumer-test") != IDYDB_SUCCESS) goto done;
    if (idydb_insert_const_char(&handle, 1, 1, "installed C consumer") != IDYDB_DONE) goto done;
    idydb_set_embedder(&handle, embed_from_consumer, &context);
    if (idydb_rag_upsert_text_auto_embed(&handle, 2, 3, 1, callback_text) != IDYDB_DONE ||
        context.calls != 1) goto done;
    if (idydb_close(&handle) != IDYDB_DONE) goto done;
    if (idydb_open_encrypted(path, &handle, IDYDB_READONLY, "consumer-test") != IDYDB_SUCCESS) goto done;
    if (idydb_extract(&handle, 1, 1) != IDYDB_DONE) goto done;
    if (idydb_retrieved_type(&handle) != IDYDB_CHAR ||
        strcmp(idydb_retrieve_char(&handle), "installed C consumer") != 0) goto done;
    if (idydb_extract(&handle, 2, 1) != IDYDB_DONE ||
        idydb_retrieved_type(&handle) != IDYDB_CHAR ||
        strcmp(idydb_retrieve_char(&handle), callback_text) != 0) goto done;
    if (idydb_extract(&handle, 3, 1) != IDYDB_DONE ||
        idydb_retrieved_type(&handle) != IDYDB_VECTOR) goto done;
    vector = idydb_retrieve_vector(&handle, &dimensions);
    if (!vector || dimensions != 2 || vector[0] != 0.25f || vector[1] != -0.75f) goto done;
    result = idydb_close(&handle) == IDYDB_DONE ? 0 : 1;
done:
    idydb_discard(&handle);
    remove(path);
    return result;
}
