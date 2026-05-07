#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "_phobic.h"

/* phobic 0.3.0 Python C extension.
 *
 * Public surface (called from phobic/__init__.py):
 *   build(keys, load_factor, seed, max_retries, bucket_size, num_shards, num_threads)
 *   query(capsule, key_bytes)
 *   query_batch(capsule, list_of_bytes, num_threads)
 *   serialize(capsule)
 *   deserialize(bytes)
 *   num_keys(capsule)
 *   range_size(capsule)
 *   num_shards(capsule)
 *   bits_per_key(capsule)
 *
 * The Python wrapper does all argument validation (load_factor in (0,1], etc.).
 * This layer just unpacks positional args, allocates flat C buffers, releases
 * the GIL for the build, and re-acquires for result packaging.
 */

static void phf_capsule_destructor(PyObject *capsule) {
    phobic_phf *phf = (phobic_phf *)PyCapsule_GetPointer(capsule, "phobic_phf");
    phobic_free(phf);
}

/* ── build ─────────────────────────────────────────────────────────── */

static PyObject *py_build(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *keys_list;
    double load_factor;
    unsigned long long seed;
    int max_retries;
    Py_ssize_t bucket_size, num_shards;
    int num_threads;

    if (!PyArg_ParseTuple(args, "O!dKinni",
                          &PyList_Type, &keys_list,
                          &load_factor,
                          &seed,
                          &max_retries,
                          &bucket_size,
                          &num_shards,
                          &num_threads))
        return NULL;

    if (bucket_size < 0 || num_shards < 0) {
        PyErr_SetString(PyExc_ValueError,
            "bucket_size and num_shards must be >= 0");
        return NULL;
    }

    Py_ssize_t n = PyList_GET_SIZE(keys_list);
    if (n == 0) {
        PyErr_SetString(PyExc_ValueError, "keys must be non-empty");
        return NULL;
    }

    const char **key_ptrs = malloc((size_t)n * sizeof(char *));
    size_t *key_lens = malloc((size_t)n * sizeof(size_t));
    if (!key_ptrs || !key_lens) {
        free(key_ptrs); free(key_lens);
        return PyErr_NoMemory();
    }

    /* Validate types and collect lengths. */
    size_t total_bytes = 0;
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *item = PyList_GET_ITEM(keys_list, i);
        if (!PyBytes_Check(item)) {
            free(key_ptrs); free(key_lens);
            PyErr_SetString(PyExc_TypeError, "all keys must be bytes");
            return NULL;
        }
        key_lens[i] = (size_t)PyBytes_GET_SIZE(item);
        if (key_lens[i] > SIZE_MAX - total_bytes) {
            free(key_ptrs); free(key_lens);
            PyErr_SetString(PyExc_OverflowError, "total key data exceeds SIZE_MAX");
            return NULL;
        }
        total_bytes += key_lens[i];
    }

    /* Copy keys to a flat C buffer so the build (under released GIL)
     * never touches Python memory. */
    char *key_data = malloc(total_bytes ? total_bytes : 1);
    if (!key_data) {
        free(key_ptrs); free(key_lens);
        return PyErr_NoMemory();
    }
    size_t offset = 0;
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *item = PyList_GET_ITEM(keys_list, i);
        memcpy(key_data + offset, PyBytes_AS_STRING(item), key_lens[i]);
        key_ptrs[i] = key_data + offset;
        offset += key_lens[i];
    }

    phobic_build_opts opts = {
        .load_factor = load_factor,
        .seed        = (uint64_t)seed,
        .max_retries = max_retries,
        .bucket_size = (size_t)bucket_size,
        .num_shards  = (size_t)num_shards,
        .num_threads = num_threads,
    };
    phobic_build_diag diag = {0};

    phobic_phf *phf;
    Py_BEGIN_ALLOW_THREADS
    phf = phobic_build_with_diag(key_ptrs, key_lens, (size_t)n, &opts, &diag);
    Py_END_ALLOW_THREADS

    free(key_data);
    free(key_ptrs);
    free(key_lens);

    if (!phf) {
        if (diag.failed_shard >= 0) {
            /* PyErr_Format / PyUnicode_FromFormatV doesn't support %f or %zu.
             * Use snprintf (which does) into a buffer, then PyErr_SetString. */
            char err[512];
            snprintf(err, sizeof(err),
                "PHOBIC build failed: shard %d could not place all keys after "
                "%d retries (best attempt left %zu unplaceable at "
                "load_factor=%.4f, bucket_size=%zu). "
                "Try lower load_factor, more shards, or higher max_retries.",
                diag.failed_shard,
                max_retries > 0 ? max_retries : 100,
                diag.best_collisions,
                diag.resolved_load_factor,
                diag.resolved_bucket_size);
            PyErr_SetString(PyExc_RuntimeError, err);
        } else {
            PyErr_SetString(PyExc_RuntimeError,
                "PHOBIC build failed (allocation or invalid input)");
        }
        return NULL;
    }

    return PyCapsule_New(phf, "phobic_phf", phf_capsule_destructor);
}

/* ── query ─────────────────────────────────────────────────────────── */

static PyObject *py_query(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *capsule;
    const char *key;
    Py_ssize_t key_len;

    if (!PyArg_ParseTuple(args, "Oy#", &capsule, &key, &key_len))
        return NULL;

    phobic_phf *phf = (phobic_phf *)PyCapsule_GetPointer(capsule, "phobic_phf");
    if (!phf) return NULL;

    size_t slot = phobic_query(phf, key, (size_t)key_len);
    return PyLong_FromSize_t(slot);
}

/* Batch query. Copies all keys into a flat buffer, releases the GIL,
 * runs the C batch query (single-threaded in 3a), then packages the
 * result into a Python list. */
static PyObject *py_query_batch(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *capsule;
    PyObject *keys_list;
    int num_threads = 0;

    if (!PyArg_ParseTuple(args, "OO!i", &capsule, &PyList_Type, &keys_list,
                          &num_threads))
        return NULL;

    phobic_phf *phf = (phobic_phf *)PyCapsule_GetPointer(capsule, "phobic_phf");
    if (!phf) return NULL;

    Py_ssize_t n = PyList_GET_SIZE(keys_list);
    if (n == 0) return PyList_New(0);

    const char **key_ptrs = malloc((size_t)n * sizeof(char *));
    size_t *key_lens = malloc((size_t)n * sizeof(size_t));
    size_t *out_slots = malloc((size_t)n * sizeof(size_t));
    if (!key_ptrs || !key_lens || !out_slots) {
        free(key_ptrs); free(key_lens); free(out_slots);
        return PyErr_NoMemory();
    }

    /* Validate types and pack into flat buffer (so we can drop the GIL). */
    size_t total_bytes = 0;
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *item = PyList_GET_ITEM(keys_list, i);
        if (!PyBytes_Check(item)) {
            free(key_ptrs); free(key_lens); free(out_slots);
            PyErr_SetString(PyExc_TypeError, "all keys must be bytes");
            return NULL;
        }
        key_lens[i] = (size_t)PyBytes_GET_SIZE(item);
        if (key_lens[i] > SIZE_MAX - total_bytes) {
            free(key_ptrs); free(key_lens); free(out_slots);
            PyErr_SetString(PyExc_OverflowError, "total key data exceeds SIZE_MAX");
            return NULL;
        }
        total_bytes += key_lens[i];
    }
    char *key_data = malloc(total_bytes ? total_bytes : 1);
    if (!key_data) {
        free(key_ptrs); free(key_lens); free(out_slots);
        return PyErr_NoMemory();
    }
    size_t offset = 0;
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *item = PyList_GET_ITEM(keys_list, i);
        memcpy(key_data + offset, PyBytes_AS_STRING(item), key_lens[i]);
        key_ptrs[i] = key_data + offset;
        offset += key_lens[i];
    }

    Py_BEGIN_ALLOW_THREADS
    phobic_query_batch(phf, key_ptrs, key_lens, (size_t)n, out_slots, num_threads);
    Py_END_ALLOW_THREADS

    PyObject *result = PyList_New(n);
    if (!result) {
        free(key_data); free(key_ptrs); free(key_lens); free(out_slots);
        return NULL;
    }
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *slot_obj = PyLong_FromSize_t(out_slots[i]);
        if (!slot_obj) {
            Py_DECREF(result);
            free(key_data); free(key_ptrs); free(key_lens); free(out_slots);
            return NULL;
        }
        PyList_SET_ITEM(result, i, slot_obj);  /* steals reference */
    }
    free(key_data); free(key_ptrs); free(key_lens); free(out_slots);
    return result;
}

/* ── serialization ─────────────────────────────────────────────────── */

static PyObject *py_serialize(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *capsule;
    if (!PyArg_ParseTuple(args, "O", &capsule)) return NULL;

    phobic_phf *phf = (phobic_phf *)PyCapsule_GetPointer(capsule, "phobic_phf");
    if (!phf) return NULL;

    size_t needed = phobic_serialize(phf, NULL, 0);
    if (needed == 0) {
        PyErr_SetString(PyExc_RuntimeError, "phobic_serialize returned 0");
        return NULL;
    }
    PyObject *bytes = PyBytes_FromStringAndSize(NULL, (Py_ssize_t)needed);
    if (!bytes) return NULL;

    size_t written = phobic_serialize(phf, (uint8_t *)PyBytes_AS_STRING(bytes), needed);
    if (written != needed) {
        Py_DECREF(bytes);
        PyErr_SetString(PyExc_RuntimeError, "phobic_serialize size mismatch");
        return NULL;
    }
    return bytes;
}

static PyObject *py_deserialize(PyObject *self, PyObject *args) {
    (void)self;
    const char *data;
    Py_ssize_t data_len;

    if (!PyArg_ParseTuple(args, "y#", &data, &data_len)) return NULL;

    phobic_phf *phf = phobic_deserialize((const uint8_t *)data, (size_t)data_len);
    if (!phf) {
        PyErr_SetString(PyExc_ValueError, "invalid serialized PHF (bad magic, version, or layout)");
        return NULL;
    }

    return PyCapsule_New(phf, "phobic_phf", phf_capsule_destructor);
}

/* ── accessors ─────────────────────────────────────────────────────── */

static PyObject *py_num_keys(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *capsule;
    if (!PyArg_ParseTuple(args, "O", &capsule)) return NULL;
    phobic_phf *phf = (phobic_phf *)PyCapsule_GetPointer(capsule, "phobic_phf");
    if (!phf) return NULL;
    return PyLong_FromSize_t(phf->num_keys);
}

static PyObject *py_range_size(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *capsule;
    if (!PyArg_ParseTuple(args, "O", &capsule)) return NULL;
    phobic_phf *phf = (phobic_phf *)PyCapsule_GetPointer(capsule, "phobic_phf");
    if (!phf) return NULL;
    return PyLong_FromSize_t(phf->total_range);
}

static PyObject *py_num_shards(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *capsule;
    if (!PyArg_ParseTuple(args, "O", &capsule)) return NULL;
    phobic_phf *phf = (phobic_phf *)PyCapsule_GetPointer(capsule, "phobic_phf");
    if (!phf) return NULL;
    return PyLong_FromSize_t(phf->num_shards);
}

static PyObject *py_bits_per_key(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *capsule;
    if (!PyArg_ParseTuple(args, "O", &capsule)) return NULL;
    phobic_phf *phf = (phobic_phf *)PyCapsule_GetPointer(capsule, "phobic_phf");
    if (!phf) return NULL;
    return PyFloat_FromDouble(phobic_bits_per_key(phf));
}

/* ── module table ──────────────────────────────────────────────────── */

static PyMethodDef module_methods[] = {
    {"build",        py_build,        METH_VARARGS, "Build a PHF from keys"},
    {"query",        py_query,        METH_VARARGS, "Query a PHF for a key's slot"},
    {"query_batch",  py_query_batch,  METH_VARARGS, "Batch query a PHF for many keys"},
    {"serialize",    py_serialize,    METH_VARARGS, "Serialize a PHF to bytes (wire format v3)"},
    {"deserialize",  py_deserialize,  METH_VARARGS, "Deserialize a PHF from bytes"},
    {"num_keys",     py_num_keys,     METH_VARARGS, "Number of keys in the PHF"},
    {"range_size",   py_range_size,   METH_VARARGS, "Total slot range"},
    {"num_shards",   py_num_shards,   METH_VARARGS, "Number of internal shards"},
    {"bits_per_key", py_bits_per_key, METH_VARARGS, "Serialized size in bits / num_keys"},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef module_def = {
    PyModuleDef_HEAD_INIT,
    "phobic._module",
    "PHOBIC perfect hash function (C extension, wire format v3)",
    -1,
    module_methods,
    NULL, NULL, NULL, NULL
};

PyMODINIT_FUNC PyInit__module(void) {
    return PyModule_Create(&module_def);
}
