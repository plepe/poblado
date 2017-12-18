#include "rapidjson_writable.h"
#include "async_worker.h"

#include <uv.h>
uint64_t START_TIME = uv_hrtime() / 1E6;
#include  "common.h"
#include <iostream>
#include <fstream>

//
// API
//

// allow overriding the assert, i.e. if you want to return an error value
// instead of aborting
#ifndef POBLADO_ASSERT
#define POBLADO_ASSERT(expr)                              \
 do {                                                     \
  if (!(expr)) {                                          \
    fprintf(stderr,                                       \
            "Assertion failed in %s on line %d: %s\n",    \
            __FILE__,                                     \
            __LINE__,                                     \
            #expr);                                       \
    abort();                                              \
  }                                                       \
 } while (0)
#endif

// by default don't log, override if you want to see log messages
#ifndef poblado_log
#define poblado_log(msg)
#endif

typedef struct {
  const char* data;
  const size_t size;
} chunk_t;

typedef struct {
  size_t chunkSize;
  std::vector<chunk_t*> chunks;
  bool done;
  uv_mutex_t mutex;
} parser_work_t;

template <typename T>
class ParseResult {
  public:
    static ParseResult* Success(T* result) {
      return new ParseResult(false, result);
    }

    static ParseResult* Failure() {
      return new ParseResult(true, nullptr);
    }

    // worry about aggregating error info later
    // https://github.com/nodesource/rapidjson-writable/blob/master/test/util.h#L55
    const bool hasError;
    T* result;

  private:
    ParseResult(bool hasError, T* result)
      : hasError(hasError), result(result) {}
};

using rapidjson_writable::RapidjsonWritable;
using rapidjson_writable::SaxHandler;

template <typename T>
class RapidjsonWritableWithResult : public RapidjsonWritable {
  public:
    RapidjsonWritableWithResult()
      : RapidjsonWritable(), parseResult_(nullptr) {}

    ParseResult<T>* const result() { return parseResult_; }

  protected:
    ParseResult<T>* parseResult_;
};

template <typename T>
class PobladoWorker : public AsyncWorker<ParseResult<T>, parser_work_t> {
  public:
    PobladoWorker(uv_loop_t* loop, parser_work_t& work, RapidjsonWritableWithResult<T>& jsonWritable)
      : AsyncWorker<ParseResult<T>, parser_work_t>(loop, work), jsonWritable_(jsonWritable) {

        // require user to do the initialization and handle potential resulting errors
        // otherwise we'll have to somehow propagate those
        POBLADO_ASSERT(jsonWritable_.initialized());
      }

    // @override
    ParseResult<T>* onwork(parser_work_t& work) {
      poblado_log("parser started work");

      bool done;
      bool chunkToProcess;
      chunk_t* chunk;
      do {
        uv_mutex_lock(&work.mutex);
        {
          poblado_log("processor reading {");
          done = work.done;
          chunkToProcess = work.chunks.size() > 0;
          if (chunkToProcess) {
            poblado_log("  will process new chunk");
            chunk = work.chunks.front();
            work.chunks.erase(work.chunks.begin());
          } else {
            poblado_log("  no chunk to process");
          }
        }
        uv_mutex_unlock(&work.mutex);
        poblado_log("}");

        if (chunkToProcess) {
          jsonWritable_.write(*chunk->data, chunk->size);
        }
      } while(!done || chunkToProcess);

      // join this worker thread with parser thread to ensure it finished
      // before we return a result
      jsonWritable_.wait();
      return jsonWritable_.result();
    }

    // @override
    void ondone(ParseResult<T>* result, int status) {
      ASSERT(status == 0);
      poblado_log("ondone");
    }

  private:
    ParseResult<T>* parseResult_;
    RapidjsonWritableWithResult<T>& jsonWritable_;
};

//
// End API
//

#define SLEEP_TIME 40
#define CHUNK_SIZE 640000


class CollectedKeys {
  public:
    void addKey(char* key) {
      keys.push_back(key);
    }

    std::vector<char*> keys;
};



using rapidjson_writable::JsonType;
class Writable : public RapidjsonWritableWithResult<CollectedKeys> {
  void onparserFailure(rapidjson::Reader& reader) {
    poblado_log("parser failure");
  }

  void onparsedToken(SaxHandler& handler) {
    poblado_log("parsed a token");
    if (handler.type == JsonType::Key) {
      ParseResult<CollectedKeys>* parseResult = this->result();
      CollectedKeys* collecteds = parseResult->result;
      collecteds->addKey(scopy(handler.stringVal.c_str()));
    }
  }

  void onparseComplete() {
    poblado_log("parser complete");
    for (auto& key : parseResult_->result->keys) {
      poblado_log(key);
    }
  }

};

class IterateLoopData {
  public:
    IterateLoopData(std::istream& stream, parser_work_t& parser_work)
      : stream(stream), parser_work(parser_work) {};

  std::istream& stream;
  parser_work_t& parser_work;
};

static void onloopIteration(uv_idle_t* handle) {
  IterateLoopData* loop_data = static_cast<IterateLoopData*>(handle->data);
  parser_work_t& loop_work = loop_data->parser_work;

  uv_mutex_lock(&loop_work.mutex);
  {
    poblado_log("loop {");
    std::vector<char> buffer(loop_work.chunkSize, 0);
    loop_data->stream.read(buffer.data(), buffer.size());
    chunk_t* chunk = new chunk_t({
      .data = copy_buffer(buffer.data(), buffer.size()),
      .size = buffer.size()
    });
    loop_work.chunks.emplace_back(chunk);
    chunk = nullptr;

    poblado_log(" added chunk, waiting to process");

    if (loop_data->stream.eof()) {
      poblado_log(" reached end of stream, stopping");
      loop_work.done = true;
      uv_idle_stop(handle);
    }
  }
  uv_mutex_unlock(&loop_work.mutex);
  poblado_log("}");

  // producing chunks twice as fast as they are processed
  uv_sleep(SLEEP_TIME / 2);
}

int main(int argc, char *argv[]) {
  uv_loop_t* loop = uv_default_loop();

  //
  // Initialize Writable
  //
  Writable jsonWritable;
  {
    const TestWritableResult* r = jsonWritable.init(TestWritableResult::OK());
    if (r->hasError) {
      fprintf(stderr, "Encountered writable init error: %s\n", r->errorMsg);
      delete r;
      return 1;
    }
    delete r;
  }

  //
  // Create File Stream and schedule writes on each loop iteration
  //
  const char* file = argv[1];
  fprintf(stderr, "Processing %s\n", file);
  std::ifstream stream(file);

  parser_work_t parser_work { .chunkSize = CHUNK_SIZE };
  IterateLoopData iterateLoopData(stream, parser_work);
  uv_idle_t idle;
  {
    int r = uv_idle_init(loop, &idle);
    ASSERT(r == 0);
    idle.data = &iterateLoopData;

    r = uv_mutex_init(&parser_work.mutex);
    ASSERT(r == 0);

    r = uv_idle_start(&idle, onloopIteration);
    ASSERT(r == 0);
  }

  //
  // Create background poblado processor and initialize it to work
  //
  PobladoWorker<CollectedKeys> pobladoWorker(loop, iterateLoopData.parser_work, jsonWritable);
  {
    int r = pobladoWorker.work();
    ASSERT(r == 0);
  }

  //
  // Start running loop
  //
  poblado_log("starting loop");
  uv_run(loop, UV_RUN_DEFAULT);

  return 0;
}
