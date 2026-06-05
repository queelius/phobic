#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "_phobic.h"

#ifdef PHOBIC_PROFILE
#include <time.h>
#include <stdio.h>
static inline double pp_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
#define PP_REPORT(label, t0) \
    fprintf(stderr, "[phobic-profile] %-40s %7.1f ms\n", \
            (label), (pp_now() - (t0)) * 1000.0)
#endif

/* phobic 0.4.0 Python C extension.
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

/* Parse a single-capsule arg tuple and return the wrapped phobic_phf*.
 * Returns NULL with a Python exception already set on any error (bad arg
 * tuple, wrong capsule type). Used by the introspection accessors and the
 * serialize prologue, which all share this exact unpacking ritual. */
static phobic_phf *phf_from_args(PyObject *args) {
    PyObject *capsule;
    if (!PyArg_ParseTuple(args, "O", &capsule)) return NULL;
    return (phobic_phf *)PyCapsule_GetPointer(capsule, "phobic_phf");
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

#ifdef PHOBIC_PROFILE
    double pp_validate_t0 = pp_now();
#endif
    /* Single pass: validate types, collect lengths, point at PyBytes data.
     *
     * Safety: PyBytes is immutable; the buffer returned by PyBytes_AS_STRING
     * stays valid until the bytes object is reclaimed. The keys_list
     * argument is borrowed from the caller's tuple, which is alive for
     * the whole duration of py_build (including across the GIL release
     * below), so each PyBytes inside keys_list is alive too. We can read
     * the bytes data with the GIL released; we just cannot call any
     * Python C API on these pointers. */
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *item = PyList_GET_ITEM(keys_list, i);
        if (!PyBytes_Check(item)) {
            free(key_ptrs); free(key_lens);
            PyErr_SetString(PyExc_TypeError, "all keys must be bytes");
            return NULL;
        }
        key_lens[i] = (size_t)PyBytes_GET_SIZE(item);
        key_ptrs[i] = PyBytes_AS_STRING(item);
    }
#ifdef PHOBIC_PROFILE
    PP_REPORT("py_build: validate + collect pass", pp_validate_t0);
    double pp_build_t0 = pp_now();
#endif

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

#ifdef PHOBIC_PROFILE
    PP_REPORT("py_build: phobic_build_with_diag (parallel C)", pp_build_t0);
#endif

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
                max_retries,
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

/* Batch query. Collects PyBytes pointers without copying (PyBytes is
 * immutable; keys_list keeps each item alive across the GIL release).
 * Releases the GIL, runs the C batch query (parallel above the
 * threshold), then packages results into a Python list. */
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

    /* Single pass: validate types, collect lengths and pointers.
     * (See py_build for the safety argument.) */
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *item = PyList_GET_ITEM(keys_list, i);
        if (!PyBytes_Check(item)) {
            free(key_ptrs); free(key_lens); free(out_slots);
            PyErr_SetString(PyExc_TypeError, "all keys must be bytes");
            return NULL;
        }
        key_lens[i] = (size_t)PyBytes_GET_SIZE(item);
        key_ptrs[i] = PyBytes_AS_STRING(item);
    }

    Py_BEGIN_ALLOW_THREADS
    phobic_query_batch(phf, key_ptrs, key_lens, (size_t)n, out_slots, num_threads);
    Py_END_ALLOW_THREADS

    PyObject *result = PyList_New(n);
    if (!result) {
        free(key_ptrs); free(key_lens); free(out_slots);
        return NULL;
    }
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *slot_obj = PyLong_FromSize_t(out_slots[i]);
        if (!slot_obj) {
            Py_DECREF(result);
            free(key_ptrs); free(key_lens); free(out_slots);
            return NULL;
        }
        PyList_SET_ITEM(result, i, slot_obj);  /* steals reference */
    }
    free(key_ptrs); free(key_lens); free(out_slots);
    return result;
}

/* Fixed-width bulk query. keys is any contiguous buffer (e.g. a numpy
 * (n, width) uint8 array, or bytes) of n*width bytes; returns a bytes object
 * of n*8 holding host-endian uint64 slots, which the Python wrapper views as
 * a numpy uint64 array. Uses only the buffer protocol (no numpy linkage), so
 * numpy stays an optional runtime dependency. Skips all per-key Python objects:
 * no PyBytes list on input, no PyLong list on output. */
static PyObject *py_query_batch_fixed(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *capsule;
    Py_buffer keys;
    Py_ssize_t width;
    int num_threads;

    if (!PyArg_ParseTuple(args, "Oy*ni", &capsule, &keys, &width, &num_threads))
        return NULL;

    phobic_phf *phf = (phobic_phf *)PyCapsule_GetPointer(capsule, "phobic_phf");
    if (!phf) { PyBuffer_Release(&keys); return NULL; }

    if (width <= 0) {
        PyBuffer_Release(&keys);
        PyErr_SetString(PyExc_ValueError, "width must be >= 1");
        return NULL;
    }
    if (keys.len % width != 0) {
        PyBuffer_Release(&keys);
        PyErr_SetString(PyExc_ValueError, "key buffer length is not a multiple of width");
        return NULL;
    }
    size_t n = (size_t)keys.len / (size_t)width;

    PyObject *out = PyBytes_FromStringAndSize(NULL, (Py_ssize_t)(n * sizeof(uint64_t)));
    if (!out) { PyBuffer_Release(&keys); return NULL; }
    uint64_t *out_slots = (uint64_t *)PyBytes_AS_STRING(out);

    /* keys.buf is pinned by the Py_buffer; out is a fresh, unshared bytes whose
     * storage we fill before returning. Both are safe to touch with the GIL
     * released, as long as no Python C-API is called in the window. */
    Py_BEGIN_ALLOW_THREADS
    phobic_query_fixed_batch(phf, (const uint8_t *)keys.buf, (size_t)width, n,
                             out_slots, num_threads);
    Py_END_ALLOW_THREADS

    PyBuffer_Release(&keys);
    return out;
}

/* ── serialization ─────────────────────────────────────────────────── */

static PyObject *py_serialize(PyObject *self, PyObject *args) {
    (void)self;
    phobic_phf *phf = phf_from_args(args);
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
    phobic_phf *phf = phf_from_args(args);
    if (!phf) return NULL;
    return PyLong_FromSize_t(phf->num_keys);
}

static PyObject *py_range_size(PyObject *self, PyObject *args) {
    (void)self;
    phobic_phf *phf = phf_from_args(args);
    if (!phf) return NULL;
    return PyLong_FromSize_t(phf->total_range);
}

static PyObject *py_num_shards(PyObject *self, PyObject *args) {
    (void)self;
    phobic_phf *phf = phf_from_args(args);
    if (!phf) return NULL;
    return PyLong_FromSize_t(phf->num_shards);
}

static PyObject *py_bits_per_key(PyObject *self, PyObject *args) {
    (void)self;
    phobic_phf *phf = phf_from_args(args);
    if (!phf) return NULL;
    return PyFloat_FromDouble(phobic_bits_per_key(phf));
}

/* ── module table ──────────────────────────────────────────────────── */

static PyMethodDef module_methods[] = {
    {"build",        py_build,        METH_VARARGS, "Build a PHF from keys"},
    {"query",        py_query,        METH_VARARGS, "Query a PHF for a key's slot"},
    {"query_batch",  py_query_batch,  METH_VARARGS, "Batch query a PHF for many keys"},
    {"query_batch_fixed", py_query_batch_fixed, METH_VARARGS,
     "Fixed-width bulk query over a contiguous buffer; returns uint64 bytes"},
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
