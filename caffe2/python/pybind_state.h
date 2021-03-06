#pragma once

#include "caffe2/core/context.h"
#include "caffe2/core/init.h"
#include "caffe2/core/logging.h"
#include "caffe2/core/net.h"
#include "caffe2/core/operator.h"
#include "caffe2/core/scope_guard.h"
#include "caffe2/core/types.h"
#include "caffe2/core/workspace.h"
#include "caffe2/proto/caffe2.pb.h"

#include <pybind11/pybind11.h>

#include <Python.h>
#define NPY_NO_DEPRECATED_API NPY_1_7_API_VERSION
#define PY_ARRAY_UNIQUE_SYMBOL caffe2_python_ARRAY_API
#include <numpy/arrayobject.h>

namespace caffe2 {

// Add methods common to both CPU and GPU mode.
void addGlobalMethods(pybind11::module& m);
// Expose Workspace, Net, Blob
void addObjectMethods(pybind11::module& m);

class BlobFetcherBase {
 public:
  virtual ~BlobFetcherBase();
  virtual pybind11::object Fetch(const Blob& blob) = 0;
};

class BlobFeederBase {
 public:
  virtual ~BlobFeederBase();
  virtual void
  Feed(const DeviceOption& option, PyArrayObject* array, Blob* blob) = 0;
};

CAFFE_DECLARE_TYPED_REGISTRY(BlobFetcherRegistry, CaffeTypeId, BlobFetcherBase);
#define REGISTER_BLOB_FETCHER(id, ...) \
  CAFFE_REGISTER_TYPED_CLASS(BlobFetcherRegistry, id, __VA_ARGS__)
inline unique_ptr<BlobFetcherBase> CreateFetcher(CaffeTypeId id) {
  return BlobFetcherRegistry()->Create(id);
}

CAFFE_DECLARE_TYPED_REGISTRY(BlobFeederRegistry, int, BlobFeederBase);
#define REGISTER_BLOB_FEEDER(device_type, ...) \
  CAFFE_REGISTER_TYPED_CLASS(BlobFeederRegistry, device_type, __VA_ARGS__)
inline unique_ptr<BlobFeederBase> CreateFeeder(int device_type) {
  return BlobFeederRegistry()->Create(device_type);
}

static_assert(
    sizeof(int) == sizeof(int32_t),
    "We make an assumption that int is always int32 for numpy "
    "type mapping.");

int CaffeToNumpyType(const TypeMeta& meta);
const TypeMeta& NumpyTypeToCaffe(int numpy_type);

template <class Context>
class TensorFetcher : public BlobFetcherBase {
 public:
  pybind11::object Fetch(const Blob& blob) override {
    const Tensor<Context>& tensor = blob.Get<Tensor<Context>>();
    Context context;
    CAFFE_ENFORCE_GE(tensor.size(), 0, "Trying to fetch unitilized tensor");
    std::vector<npy_intp> npy_dims;
    for (const auto dim : tensor.dims()) {
      npy_dims.push_back(dim);
    }
    int numpy_type = CaffeToNumpyType(tensor.meta());
    CAFFE_ENFORCE(
        numpy_type != -1,
        "This tensor's data type is not supported: ",
        tensor.meta().name(),
        ".");
    PyObject* array =
        PyArray_SimpleNew(tensor.ndim(), npy_dims.data(), numpy_type);
    void* outPtr = static_cast<void*>(
        PyArray_DATA(reinterpret_cast<PyArrayObject*>(array)));

    if (numpy_type == NPY_OBJECT) {
      PyObject** outObj = reinterpret_cast<PyObject**>(outPtr);
      auto* str = tensor.template data<std::string>();
      for (int i = 0; i < tensor.size(); ++i) {
        outObj[i] = PyBytes_FromStringAndSize(str->data(), str->size());
        str++;
        // cleanup on failure
        if (outObj[i] == nullptr) {
          for (int j = 0; j < i; ++j) {
            Py_DECREF(outObj[j]);
          }
          Py_DECREF(array);
          CAFFE_THROW("Failed to allocate string for ndarray of strings.");
        }
      }
      // TODO - is this refcounted correctly?
      return pybind11::object(array, /* borrowed= */ false);
    }

    // Now, copy the data to the tensor.
    // TODO(Yangqing): Right now, to make things consistent between CPU and
    // GPU, we always do a data copy. This is not necessary for CPU and
    // read-only cases, so we may want to make it a non-copy.
    context.template CopyBytes<Context, CPUContext>(
        tensor.nbytes(), tensor.raw_data(), outPtr);
    context.FinishDeviceComputation();
    return pybind11::object(array, /* borrowed= */ false);
  }
};

template <class Context>
class TensorFeeder : public BlobFeederBase {
 public:
  virtual void
  Feed(const DeviceOption& option, PyArrayObject* original_array, Blob* blob) {
    PyArrayObject* array = PyArray_GETCONTIGUOUS(original_array);
    auto g = MakeGuard([&]() { Py_XDECREF(array); });

    const auto npy_type = PyArray_TYPE(array);
    const TypeMeta& meta = NumpyTypeToCaffe(npy_type);
    CAFFE_ENFORCE(
        meta.id() != 0,
        "This numpy data type is not supported: ",
        PyArray_TYPE(array),
        ".");
    Context context(option);
    context.SwitchToDevice();
    Tensor<Context>* tensor = blob->GetMutable<Tensor<Context>>();
    // numpy requires long int as its dims.
    int ndim = PyArray_NDIM(array);
    npy_intp* npy_dims = PyArray_DIMS(array);
    std::vector<TIndex> dims;
    for (int i = 0; i < ndim; ++i) {
      dims.push_back(npy_dims[i]);
    }
    tensor->Resize(dims);

    // Now, copy the data to the tensor.
    switch (npy_type) {
      case NPY_OBJECT: {
        PyObject** input = reinterpret_cast<PyObject**>(PyArray_DATA(array));
        auto* outPtr = tensor->template mutable_data<std::string>();
        for (int i = 0; i < tensor->size(); ++i) {
          char* str;
          Py_ssize_t strSize;
          CAFFE_ENFORCE(
              PyBytes_AsStringAndSize(input[i], &str, &strSize) != -1,
              "Unsupported python object type passed into ndarray.");
          outPtr[i] = std::string(str, strSize);
        }
      } break;
      default:
        context.template CopyBytes<CPUContext, Context>(
            tensor->size() * meta.itemsize(),
            static_cast<void*>(PyArray_DATA(array)),
            tensor->raw_mutable_data(meta));
    }
    context.FinishDeviceComputation();
  }
};
}
