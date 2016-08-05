#include "worker.h"

#include <chrono>
#include <thread>

#include "utils.h"

extern "C" {
  static PyObject *RayError;
}

inline WorkerServiceImpl::WorkerServiceImpl(const std::string& worker_address)
  : worker_address_(worker_address) {
  RAY_CHECK(send_queue_.connect(worker_address_, false), "error connecting send_queue_");
}

Status WorkerServiceImpl::ExecuteTask(ServerContext* context, const ExecuteTaskRequest* request, ExecuteTaskReply* reply) {
  RAY_LOG(RAY_INFO, "invoked task " << request->task().name());
  std::unique_ptr<WorkerMessage> message(new WorkerMessage());
  message->mutable_task()->CopyFrom(request->task());
  {
    WorkerMessage* message_ptr = message.get();
    RAY_CHECK(send_queue_.send(&message_ptr), "error sending over IPC");
  }
  message.release();
  return Status::OK;
}

Status WorkerServiceImpl::ImportFunction(ServerContext* context, const ImportFunctionRequest* request, ImportFunctionReply* reply) {
  std::unique_ptr<WorkerMessage> message(new WorkerMessage());
  message->mutable_function()->CopyFrom(request->function());
  RAY_LOG(RAY_INFO, "importing function");
  {
    WorkerMessage* message_ptr = message.get();
    RAY_CHECK(send_queue_.send(&message_ptr), "error sending over IPC");
  }
  message.release();
  return Status::OK;
}

Status WorkerServiceImpl::ImportReusableVariable(ServerContext* context, const ImportReusableVariableRequest* request, AckReply* reply) {
  std::unique_ptr<WorkerMessage> message(new WorkerMessage());
  message->mutable_reusable_variable()->CopyFrom(request->reusable_variable());
  RAY_LOG(RAY_INFO, "importing reusable variable");
  {
    WorkerMessage* message_ptr = message.get();
    RAY_CHECK(send_queue_.send(&message_ptr), "error sending over IPC");
  }
  message.release();
  return Status::OK;
}

Status WorkerServiceImpl::Die(ServerContext* context, const DieRequest* request, DieReply* reply) {
  WorkerMessage* message_ptr = NULL;
  RAY_CHECK(send_queue_.send(&message_ptr), "error sending over IPC");
  return Status::OK;
}

Worker::Worker(const std::string& scheduler_address)
    : scheduler_address_(scheduler_address) {
  auto scheduler_channel = grpc::CreateChannel(scheduler_address, grpc::InsecureChannelCredentials());
  scheduler_stub_ = Scheduler::NewStub(scheduler_channel);
}

SubmitTaskReply Worker::submit_task(SubmitTaskRequest* request, int max_retries, int retry_wait_milliseconds) {
  RAY_CHECK(connected_, "Attempted to perform submit_task but failed.");
  SubmitTaskReply reply;
  Status status;
  request->set_workerid(workerid_);
  for (int i = 0; i < 1 + max_retries; ++i) {
    ClientContext context;
    status = scheduler_stub_->SubmitTask(&context, *request, &reply);
    if (reply.function_registered()) {
      break;
    }
    RAY_LOG(RAY_INFO, "The function " << request->task().name() << " was not registered, so attempting to resubmit the task.");
    std::this_thread::sleep_for(std::chrono::milliseconds(retry_wait_milliseconds));
  }
  return reply;
}

bool Worker::kill_workers(ClientContext &context) {
  KillWorkersRequest request;
  KillWorkersReply reply;
  Status status = scheduler_stub_->KillWorkers(&context, request, &reply);
  return reply.success();
}

void Worker::register_worker(const std::string& node_ip_address, const std::string& objstore_address, bool is_driver) {
  unsigned int retry_wait_milliseconds = 20;
  RegisterWorkerRequest request;
  request.set_node_ip_address(node_ip_address);
  // The object store address can be the empty string, in which case the
  // scheduler will assign an object store address.
  request.set_objstore_address(objstore_address);
  request.set_is_driver(is_driver);
  RegisterWorkerReply reply;
  grpc::StatusCode status_code = grpc::UNAVAILABLE;
  // TODO: HACK: retrying is a hack
  for (int i = 0; i < 5; ++i) {
    ClientContext context;
    status_code = scheduler_stub_->RegisterWorker(&context, request, &reply).error_code();
    if (status_code != grpc::UNAVAILABLE) {
      break;
    }
    // Note that each pass through the loop may take substantially longer than
    // retry_wait_milliseconds because grpc may do its own retrying.
    std::this_thread::sleep_for(std::chrono::milliseconds(retry_wait_milliseconds));
  }
  workerid_ = reply.workerid();
  objstoreid_ = reply.objstoreid();
  objstore_address_ = reply.objstore_address();
  worker_address_ = reply.worker_address();
  segmentpool_ = std::make_shared<MemorySegmentPool>(objstoreid_, false);
  RAY_CHECK(receive_queue_.connect(worker_address_, true), "error connecting receive_queue_");
  RAY_CHECK(request_obj_queue_.connect(std::string("queue:") + objstore_address_ + std::string(":obj"), false), "error connecting request_obj_queue_");
  RAY_CHECK(receive_obj_queue_.connect(std::string("queue:") + objstore_address_ + std::string(":worker:") + std::to_string(workerid_) + std::string(":obj"), true), "error connecting receive_obj_queue_");
  connected_ = true;
  return;
}

void Worker::request_object(ObjectID objectid) {
  RAY_CHECK(connected_, "Attempted to perform request_object but failed.");
  RequestObjRequest request;
  request.set_workerid(workerid_);
  request.set_objectid(objectid);
  AckReply reply;
  ClientContext context;
  Status status = scheduler_stub_->RequestObj(&context, request, &reply);
  return;
}

ObjectID Worker::get_objectid() {
  // first get objectid for the new object
  RAY_CHECK(connected_, "Attempted to perform get_objectid but failed.");
  PutObjRequest request;
  request.set_workerid(workerid_);
  PutObjReply reply;
  ClientContext context;
  Status status = scheduler_stub_->PutObj(&context, request, &reply);
  return reply.objectid();
}

slice Worker::get_object(ObjectID objectid) {
  // get_object assumes that objectid is a canonical objectid
  RAY_CHECK(connected_, "Attempted to perform get_object but failed.");
  ObjRequest request;
  request.workerid = workerid_;
  request.type = ObjRequestType::GET;
  request.objectid = objectid;
  RAY_CHECK(request_obj_queue_.send(&request), "error sending over IPC");
  ObjHandle result;
  RAY_CHECK(receive_obj_queue_.receive(&result), "error receiving over IPC");
  slice slice;
  slice.data = segmentpool_->get_address(result);
  slice.len = result.size();
  slice.segmentid = result.segmentid();
  return slice;
}

// TODO(pcm): More error handling
// contained_objectids is a vector of all the objectids contained in obj
void Worker::put_object(ObjectID objectid, const Obj* obj, std::vector<ObjectID> &contained_objectids) {
  RAY_CHECK(connected_, "Attempted to perform put_object but failed.");
  std::string data;
  obj->SerializeToString(&data); // TODO(pcm): get rid of this serialization
  ObjRequest request;
  request.workerid = workerid_;
  request.type = ObjRequestType::ALLOC;
  request.objectid = objectid;
  request.size = data.size();
  RAY_CHECK(request_obj_queue_.send(&request), "error sending over IPC");
  if (contained_objectids.size() > 0) {
    RAY_LOG(RAY_REFCOUNT, "In put_object, calling increment_reference_count for contained objectids");
    increment_reference_count(contained_objectids); // Notify the scheduler that some object references are serialized in the objstore.
  }
  ObjHandle result;
  RAY_CHECK(receive_obj_queue_.receive(&result), "error receiving over IPC");
  uint8_t* target = segmentpool_->get_address(result);
  std::memcpy(target, data.data(), data.size());
  // We immediately unmap here; if the object is going to be accessed again, it will be mapped again;
  // This is reqired because we do not have a mechanism to unmap the object later.
  segmentpool_->unmap_segment(result.segmentid());
  request.type = ObjRequestType::WORKER_DONE;
  request.metadata_offset = 0;
  RAY_CHECK(request_obj_queue_.send(&request), "error sending over IPC");

  // Notify the scheduler about the objectids that we are serializing in the objstore.
  AddContainedObjectIDsRequest contained_objectids_request;
  contained_objectids_request.set_objectid(objectid);
  for (int i = 0; i < contained_objectids.size(); ++i) {
    contained_objectids_request.add_contained_objectid(contained_objectids[i]); // TODO(rkn): The naming here is bad
  }
  AckReply reply;
  ClientContext context;
  scheduler_stub_->AddContainedObjectIDs(&context, contained_objectids_request, &reply);
}

#define CHECK_ARROW_STATUS(s, msg)                              \
  do {                                                          \
    arrow::Status _s = (s);                                     \
    if (!_s.ok()) {                                             \
      std::string _errmsg = std::string(msg) + _s.ToString();   \
      PyErr_SetString(RayError, _errmsg.c_str());            \
      return NULL;                                              \
    }                                                           \
  } while (0);

const char* Worker::allocate_buffer(ObjectID objectid, int64_t size, SegmentId& segmentid) {
  RAY_CHECK(connected_, "Attempted to perform put_arrow but failed.");
  ObjRequest request;
  request.workerid = workerid_;
  request.type = ObjRequestType::ALLOC;
  request.objectid = objectid;
  request.size = size;
  RAY_CHECK(request_obj_queue_.send(&request), "error sending over IPC");
  ObjHandle result;
  RAY_CHECK(receive_obj_queue_.receive(&result), "error receiving over IPC");
  const char* address = reinterpret_cast<const char*>(segmentpool_->get_address(result));
  segmentid = result.segmentid();
  return address;
}

PyObject* Worker::finish_buffer(ObjectID objectid, SegmentId segmentid, int64_t metadata_offset) {
  segmentpool_->unmap_segment(segmentid);
  ObjRequest request;
  request.workerid = workerid_;
  request.objectid = objectid;
  request.type = ObjRequestType::WORKER_DONE;
  request.metadata_offset = metadata_offset;
  RAY_CHECK(request_obj_queue_.send(&request), "error sending over IPC");
  Py_RETURN_NONE;
}

const char* Worker::get_buffer(ObjectID objectid, int64_t &size, SegmentId& segmentid, int64_t& metadata_offset) {
  RAY_CHECK(connected_, "Attempted to perform get_arrow but failed.");
  ObjRequest request;
  request.workerid = workerid_;
  request.type = ObjRequestType::GET;
  request.objectid = objectid;
  RAY_CHECK(request_obj_queue_.send(&request), "error sending over IPC");
  ObjHandle result;
  RAY_CHECK(receive_obj_queue_.receive(&result), "error receiving over IPC");
  const char* address = reinterpret_cast<const char*>(segmentpool_->get_address(result));
  size = result.size();
  segmentid = result.segmentid();
  metadata_offset = result.metadata_offset();
  return address;
}

bool Worker::is_arrow(ObjectID objectid) {
  RAY_CHECK(connected_, "Attempted to perform is_arrow but failed.");
  ObjRequest request;
  request.workerid = workerid_;
  request.type = ObjRequestType::GET;
  request.objectid = objectid;
  request_obj_queue_.send(&request);
  ObjHandle result;
  RAY_CHECK(receive_obj_queue_.receive(&result), "error receiving over IPC");
  return result.metadata_offset() != 0;
}

void Worker::unmap_object(ObjectID objectid) {
  if (!connected_) {
    RAY_LOG(RAY_DEBUG, "Attempted to perform unmap_object but failed.");
    return;
  }
  segmentpool_->unmap_segment(objectid);
}

void Worker::alias_objectids(ObjectID alias_objectid, ObjectID target_objectid) {
  RAY_CHECK(connected_, "Attempted to perform alias_objectids but failed.");
  ClientContext context;
  AliasObjectIDsRequest request;
  request.set_alias_objectid(alias_objectid);
  request.set_target_objectid(target_objectid);
  AckReply reply;
  scheduler_stub_->AliasObjectIDs(&context, request, &reply);
}

void Worker::increment_reference_count(std::vector<ObjectID> &objectids) {
  if (!connected_) {
    RAY_LOG(RAY_DEBUG, "Attempting to increment_reference_count for objectids, but connected_ = " << connected_ << " so returning instead.");
    return;
  }
  if (objectids.size() > 0) {
    ClientContext context;
    IncrementRefCountRequest request;
    for (int i = 0; i < objectids.size(); ++i) {
      RAY_LOG(RAY_REFCOUNT, "Incrementing reference count for objectid " << objectids[i]);
      request.add_objectid(objectids[i]);
    }
    AckReply reply;
    scheduler_stub_->IncrementRefCount(&context, request, &reply);
  }
}

void Worker::decrement_reference_count(std::vector<ObjectID> &objectids) {
  if (!connected_) {
    RAY_LOG(RAY_DEBUG, "Attempting to decrement_reference_count, but connected_ = " << connected_ << " so returning instead.");
    return;
  }
  if (objectids.size() > 0) {
    ClientContext context;
    DecrementRefCountRequest request;
    for (int i = 0; i < objectids.size(); ++i) {
      RAY_LOG(RAY_REFCOUNT, "Decrementing reference count for objectid " << objectids[i]);
      request.add_objectid(objectids[i]);
    }
    AckReply reply;
    scheduler_stub_->DecrementRefCount(&context, request, &reply);
  }
}

void Worker::register_function(const std::string& name, size_t num_return_vals) {
  RAY_CHECK(connected_, "Attempted to perform register_function but failed.");
  ClientContext context;
  RegisterFunctionRequest request;
  request.set_fnname(name);
  request.set_num_return_vals(num_return_vals);
  request.set_workerid(workerid_);
  AckReply reply;
  scheduler_stub_->RegisterFunction(&context, request, &reply);
}

std::unique_ptr<WorkerMessage> Worker::receive_next_message() {
  WorkerMessage* message_ptr;
  RAY_CHECK(receive_queue_.receive(&message_ptr), "error receiving over IPC");
  return std::unique_ptr<WorkerMessage>(message_ptr);
}

void Worker::notify_task_completed(bool task_succeeded, std::string error_message) {
  RAY_CHECK(connected_, "Attempted to perform notify_task_completed but failed.");
  ClientContext context;
  ReadyForNewTaskRequest request;
  request.set_workerid(workerid_);
  ReadyForNewTaskRequest::PreviousTaskInfo* previous_task_info = request.mutable_previous_task_info();
  previous_task_info->set_task_succeeded(task_succeeded);
  previous_task_info->set_error_message(error_message);
  AckReply reply;
  scheduler_stub_->ReadyForNewTask(&context, request, &reply);
}

void Worker::disconnect() {
  connected_ = false;
}

bool Worker::connected() {
  return connected_;
}

// TODO(rkn): Should we be using pointers or references? And should they be const?
void Worker::scheduler_info(ClientContext &context, SchedulerInfoRequest &request, SchedulerInfoReply &reply) {
  RAY_CHECK(connected_, "Attempted to get scheduler info but failed.");
  scheduler_stub_->SchedulerInfo(&context, request, &reply);
}

void Worker::task_info(ClientContext &context, TaskInfoRequest &request, TaskInfoReply &reply) {
  RAY_CHECK(connected_, "Attempted to get worker info but failed.");
  scheduler_stub_->TaskInfo(&context, request, &reply);
}

bool Worker::export_function(const std::string& function) {
  RAY_CHECK(connected_, "Attempted to export function but failed.");
  ClientContext context;
  ExportFunctionRequest request;
  request.mutable_function()->set_implementation(function);
  ExportFunctionReply reply;
  Status status = scheduler_stub_->ExportFunction(&context, request, &reply);
  return true;
}

void Worker::export_reusable_variable(const std::string& name, const std::string& initializer, const std::string& reinitializer) {
  RAY_CHECK(connected_, "Attempted to export reusable variable but failed.");
  ClientContext context;
  ExportReusableVariableRequest request;
  request.mutable_reusable_variable()->set_name(name);
  request.mutable_reusable_variable()->mutable_initializer()->set_implementation(initializer);
  request.mutable_reusable_variable()->mutable_reinitializer()->set_implementation(reinitializer);
  AckReply reply;
  Status status = scheduler_stub_->ExportReusableVariable(&context, request, &reply);
}

// Communication between the WorkerServer and the Worker happens via a message
// queue. This is because the Python interpreter needs to be single threaded
// (in our case running in the main thread), whereas the WorkerService will
// run in a separate thread and potentially utilize multiple threads.
void Worker::start_worker_service() {
  const char* service_addr = worker_address_.c_str();
  worker_server_thread_ = std::thread([this, service_addr]() {
    std::string service_address(service_addr);
    std::string::iterator split_point = split_ip_address(service_address);
    std::string port;
    port.assign(split_point, service_address.end());
    WorkerServiceImpl service(service_address);
    ServerBuilder builder;
    builder.AddListeningPort(std::string("0.0.0.0:") + port, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    std::unique_ptr<Server> server(builder.BuildAndStart());
    RAY_LOG(RAY_INFO, "worker server listening on " << service_address);

    ClientContext context;
    ReadyForNewTaskRequest request;
    request.set_workerid(workerid_);
    AckReply reply;
    scheduler_stub_->ReadyForNewTask(&context, request, &reply);

    server->Wait();
  });
}
