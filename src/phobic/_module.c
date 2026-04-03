#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "_phobic.h"

static void phf_capsule_destructor(PyObject *capsule) {
    phobic_phf *phf = (phobic_phf *)PyCapsule_GetPointer(capsule, "phobic_phf");
    phobic_free(phf);
}

static PyObject *py_build(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *keys_list;
    double alpha;
    unsigned long long seed;

    int max_retries = 100;
    int strict = 1;

    if (!PyArg_ParseTuple(args, "O!dKii", &PyList_Type, &keys_list, &alpha, &seed,
                          &max_retries, &strict))
        return NULL;

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

    /* First pass: validate types and collect lengths. */
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

    /* Copy key data into a flat C buffer before releasing the GIL,
     * so the build call never touches Python-owned memory. */
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

    phobic_phf *phf;
    Py_BEGIN_ALLOW_THREADS
    phf = phobic_build(key_ptrs, key_lens, (size_t)n, alpha, (uint64_t)seed,
                       max_retries, strict);
    Py_END_ALLOW_THREADS

    free(key_data);
    free(key_ptrs);
    free(key_lens);

    if (!phf) {
        PyErr_SetString(PyExc_RuntimeError, "PHOBIC build failed");
        return NULL;
    }

    return PyCapsule_New(phf, "phobic_phf", phf_capsule_destructor);
}

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

static PyObject *py_serialize(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *capsule;
    if (!PyArg_ParseTuple(args, "O", &capsule)) return NULL;

    phobic_phf *phf = (phobic_phf *)PyCapsule_GetPointer(capsule, "phobic_phf");
    if (!phf) return NULL;

    size_t needed = phobic_serialize(phf, NULL, 0);
    PyObject *bytes = PyBytes_FromStringAndSize(NULL, (Py_ssize_t)needed);
    if (!bytes) return NULL;

    phobic_serialize(phf, (uint8_t *)PyBytes_AS_STRING(bytes), needed);
    return bytes;
}

static PyObject *py_deserialize(PyObject *self, PyObject *args) {
    (void)self;
    const char *data;
    Py_ssize_t data_len;

    if (!PyArg_ParseTuple(args, "y#", &data, &data_len)) return NULL;

    phobic_phf *phf = phobic_deserialize((const uint8_t *)data, (size_t)data_len);
    if (!phf) {
        PyErr_SetString(PyExc_ValueError, "invalid serialized data");
        return NULL;
    }

    return PyCapsule_New(phf, "phobic_phf", phf_capsule_destructor);
}

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
    return PyLong_FromSize_t(phf->range_size);
}

static PyObject *py_bits_per_key(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *capsule;
    if (!PyArg_ParseTuple(args, "O", &capsule)) return NULL;
    phobic_phf *phf = (phobic_phf *)PyCapsule_GetPointer(capsule, "phobic_phf");
    if (!phf) return NULL;
    return PyFloat_FromDouble(phobic_bits_per_key(phf));
}

static PyObject *py_collisions(PyObject *self, PyObject *args) {
    (void)self;
    PyObject *capsule;
    if (!PyArg_ParseTuple(args, "O", &capsule)) return NULL;
    phobic_phf *phf = (phobic_phf *)PyCapsule_GetPointer(capsule, "phobic_phf");
    if (!phf) return NULL;
    return PyLong_FromSize_t(phf->collisions);
}

static PyMethodDef module_methods[] = {
    {"build",       py_build,       METH_VARARGS, "Build a PHF from keys"},
    {"query",       py_query,       METH_VARARGS, "Query a PHF for a key's slot"},
    {"serialize",   py_serialize,   METH_VARARGS, "Serialize a PHF to bytes"},
    {"deserialize", py_deserialize, METH_VARARGS, "Deserialize a PHF from bytes"},
    {"num_keys",    py_num_keys,    METH_VARARGS, "Get number of keys"},
    {"range_size",  py_range_size,  METH_VARARGS, "Get range size"},
    {"bits_per_key",py_bits_per_key,METH_VARARGS, "Get bits per key"},
    {"collisions",  py_collisions,  METH_VARARGS, "Get collision count (0 for perfect PHF)"},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef module_def = {
    PyModuleDef_HEAD_INIT,
    "phobic._module",
    "PHOBIC perfect hash function C extension",
    -1,
    module_methods,
};

PyMODINIT_FUNC PyInit__module(void) {
    return PyModule_Create(&module_def);
}
