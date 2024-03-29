/*
 * Copyright (c) 2001, 2012, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "gc_implementation/concurrentMarkSweep/concurrentMarkSweepGeneration.hpp"
#include "gc_implementation/parNew/parGCAllocBuffer.hpp"
#include "gc_implementation/parNew/parNewGeneration.hpp"
#include "gc_implementation/parNew/parOopClosures.inline.hpp"
#include "gc_implementation/shared/adaptiveSizePolicy.hpp"
#include "gc_implementation/shared/ageTable.hpp"
#include "gc_implementation/shared/spaceDecorator.hpp"
#include "memory/defNewGeneration.inline.hpp"
#include "memory/genCollectedHeap.hpp"
#include "memory/genOopClosures.inline.hpp"
#include "memory/generation.hpp"
#include "memory/generation.inline.hpp"
#include "memory/referencePolicy.hpp"
#include "memory/resourceArea.hpp"
#include "memory/sharedHeap.hpp"
#include "memory/space.hpp"
#include "oops/objArrayOop.hpp"
#include "oops/oop.inline.hpp"
#include "oops/oop.pcgc.inline.hpp"
#include "runtime/handles.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/java.hpp"
#include "runtime/thread.hpp"
#include "utilities/copy.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/workgroup.hpp"

#ifdef _MSC_VER
#pragma warning( push )
#pragma warning( disable:4355 ) // 'this' : used in base member initializer list
#endif

ParScanThreadState::ParScanThreadState(Space* to_space_,
                                       ParNewGeneration* gen_,
                                       Generation* old_gen_,
                                       int thread_num_,
                                       ObjToScanQueueSet* work_queue_set_,
                                       Stack<oop>* overflow_stacks_,
                                       size_t desired_plab_sz_,
                                       ParallelTaskTerminator& term_) :
  _to_space(to_space_), _old_gen(old_gen_), _young_gen(gen_), _thread_num(thread_num_),
  _work_queue(work_queue_set_->queue(thread_num_)), _to_space_full(false),
  _overflow_stack(overflow_stacks_ ? overflow_stacks_ + thread_num_ : NULL),
  _ageTable(false), // false ==> not the global age table, no perf data.
  _to_space_alloc_buffer(desired_plab_sz_),
  _to_space_closure(gen_, this), _old_gen_closure(gen_, this),
  _to_space_root_closure(gen_, this), _old_gen_root_closure(gen_, this),
  _older_gen_closure(gen_, this),
  _evacuate_followers(this, &_to_space_closure, &_old_gen_closure,
                      &_to_space_root_closure, gen_, &_old_gen_root_closure,
                      work_queue_set_, &term_),
  _is_alive_closure(gen_), _scan_weak_ref_closure(gen_, this),
  _keep_alive_closure(&_scan_weak_ref_closure),
  _promotion_failure_size(0),
  _strong_roots_time(0.0), _term_time(0.0)
{
  #if TASKQUEUE_STATS
  _term_attempts = 0;
  _overflow_refills = 0;
  _overflow_refill_objs = 0;
  #endif // TASKQUEUE_STATS

  _survivor_chunk_array = (ChunkArray*) old_gen()->get_data_recorder(thread_num());
  _hash_seed = 17;  // Might want to take time-based random value.
  _start = os::elapsedTime();
  _old_gen_closure.set_generation(old_gen_);
  _old_gen_root_closure.set_generation(old_gen_);
}
#ifdef _MSC_VER
#pragma warning( pop )
#endif

void ParScanThreadState::record_survivor_plab(HeapWord* plab_start,
                                              size_t plab_word_size) {
  ChunkArray* sca = survivor_chunk_array();
  if (sca != NULL) {
    // A non-null SCA implies that we want the PLAB data recorded.
    sca->record_sample(plab_start, plab_word_size);
  }
}

/**
 * 判断指定的对象(大数组对象)是否需要分步扫描复制其引用对象
 *    1.数组对象
 *    2.数组长度>ParGCArrayScanChunk
 *    3.对象被成功复制
 */
bool ParScanThreadState::should_be_partially_scanned(oop new_obj, oop old_obj) const {
  return new_obj->is_objArray() &&
         arrayOop(new_obj)->length() > ParGCArrayScanChunk &&
         new_obj != old_obj;
}

/**
 * 分步扫描复制大数组对象的引用对象
 */
void ParScanThreadState::scan_partial_array_and_push_remainder(oop old) {
  assert(old->is_objArray(), "must be obj array");
  assert(old->is_forwarded(), "must be forwarded");
  assert(Universe::heap()->is_in_reserved(old), "must be in heap.");
  assert(!old_gen()->is_in(old), "must be in young generation.");

  objArrayOop obj = objArrayOop(old->forwardee());
  // Process ParGCArrayScanChunk elements now
  // and push the remainder back onto queue
  int start     = arrayOop(old)->length();
  int end       = obj->length();
  int remainder = end - start;
  assert(start <= end, "just checking");

  if (remainder > 2 * ParGCArrayScanChunk) {
    // Test above combines last partial chunk with a full chunk
    end = start + ParGCArrayScanChunk;
    arrayOop(old)->set_length(end);
    // Push remainder.
    bool ok = work_queue()->push(old);
    assert(ok, "just popped, push must be okay");
  } else {
    // Restore length so that it can be used if there
    // is a promotion failure and forwarding pointers
    // must be removed.
    arrayOop(old)->set_length(end);
  }

  // process our set of indices (include header in first chunk)
  // should make sure end is even (aligned to HeapWord in case of compressed oops)
  if ((HeapWord *)obj < young_old_boundary()) {
	  //复制对象的所有属性到年青代的To区
    obj->oop_iterate_range(&_to_space_closure, start, end);
  } else {
    //复制对象的所有属性到老年代
    obj->oop_iterate_range(&_old_gen_closure, start, end);
  }
}

/**
 * 如果待复制队列太满，则处理其中部分待扫描复制的对象
 */
void ParScanThreadState::trim_queues(int max_size) {
  ObjToScanQueue* queue = work_queue();

  do {
    while (queue->size() > (juint)max_size) {
      oop obj_to_scan;
      if (queue->pop_local(obj_to_scan)) {
        if ((HeapWord *)obj_to_scan < young_old_boundary()) { //待处理对象在年青代中
          if (obj_to_scan->is_objArray() &&
              obj_to_scan->is_forwarded() &&
              obj_to_scan->forwardee() != obj_to_scan) {
            scan_partial_array_and_push_remainder(obj_to_scan);
          } else {
            //将待处理对象的所有属性复制到年青代的To区
            obj_to_scan->oop_iterate(&_to_space_closure);
          }
        } else { //待处理对象在老年代中
          ///将待处理对象的所有属性复制到老年代
          obj_to_scan->oop_iterate(&_old_gen_closure);
        }
      }
    }
    // For the  case of compressed oops, we have a private, non-shared
    // overflow stack, so we eagerly drain it so as to more evenly
    // distribute load early. Note: this may be good to do in
    // general rather than delay for the final stealing phase.
    // If applicable, we'll transfer a set of objects over to our
    // work queue, allowing them to be stolen and draining our
    // private overflow stack.
  } while (ParGCTrimOverflow && young_gen()->take_from_overflow_list(this));
}

/**
 * 临时溢出栈中取出若干可处理对象转存储到工作队列中
 */
bool ParScanThreadState::take_from_overflow_stack() {
  assert(ParGCUseLocalOverflow, "Else should not call");
  assert(young_gen()->overflow_list() == NULL, "Error");
  ObjToScanQueue* queue = work_queue();
  Stack<oop>* const of_stack = overflow_stack();

  //计算转存储对象的数量
  const size_t num_overflow_elems = of_stack->size();
  const size_t space_available = queue->max_elems() - queue->size();
  const size_t num_take_elems = MIN3(space_available / 4, ParGCDesiredObjsFromOverflowList, num_overflow_elems);

  //依次从临时溢出栈中取出对象并转存到工作队列中
  for (size_t i = 0; i != num_take_elems; i++) {
    oop cur = of_stack->pop();
    oop obj_to_push = cur->forwardee();

    assert(Universe::heap()->is_in_reserved(cur), "Should be in heap");
    assert(!old_gen()->is_in_reserved(cur), "Should be in young gen");
    assert(Universe::heap()->is_in_reserved(obj_to_push), "Should be in heap");

    if (should_be_partially_scanned(obj_to_push, cur)) {
      assert(arrayOop(cur)->length() == 0, "entire array remaining to be scanned");
      obj_to_push = cur;
    }
    bool ok = queue->push(obj_to_push);
    assert(ok, "Should have succeeded");
  }
  assert(young_gen()->overflow_list() == NULL, "Error");

  return num_take_elems > 0;  // was something transferred?
}

/**
 * 将待扫描复制其引用对象的对象放到临时溢出栈中
 */
void ParScanThreadState::push_on_overflow_stack(oop p) {
  assert(ParGCUseLocalOverflow, "Else should not call");
  overflow_stack()->push(p);
  assert(young_gen()->overflow_list() == NULL, "Error");
}

/**
 * 从To区中分配指定大小的一块内存
 *   1.如果To区满，则放弃分配
 *   2.如果待分配的内存大小<(允许的)内存Buffer的碎片大小，则申请新的一块内存Buffer来分配
 *   3.直接从年青代的To区分配
 */
HeapWord* ParScanThreadState::alloc_in_to_space_slow(size_t word_sz) {

  // Otherwise, if the object is small enough, try to reallocate the
  // buffer.
  HeapWord* obj = NULL;

  if (!_to_space_full) {
    ParGCAllocBuffer* const plab = to_space_alloc_buffer();
    Space*            const sp   = to_space();

    //为当前Gc线程新申请一块内存Buffer,并从中分配指定大小的内存块
    if (word_sz * 100 < ParallelGCBufferWastePct * plab->word_sz()) {
      // Is small enough; abandon this buffer and start a new one.
      plab->retire(false, false);

      size_t buf_size = plab->word_sz();	//一个Gc线程本地Buffer的标准大小

      //从To区申请一个Gc线程本地Buffer
      HeapWord* buf_space = sp->par_allocate(buf_size);
      if (buf_space == NULL) {	//申请失败,则调整Buffer大小再尝试申请
        const size_t min_bytes = ParGCAllocBuffer::min_size() << LogHeapWordSize;	//一个Gc线程本地Buffer的最小大小
        //To区当前的空闲空间
        size_t free_bytes = sp->free();
        while(buf_space == NULL && free_bytes >= min_bytes) {	//尝试申请当前To区所有的空闲空间为一个Gc线程本地Buffer
          buf_size = free_bytes >> LogHeapWordSize;
          assert(buf_size == (size_t)align_object_size(buf_size), "Invariant");
          buf_space  = sp->par_allocate(buf_size);
          free_bytes = sp->free();
        }
      }

      if (buf_space != NULL) {	//成功申请到了一个Gc线程本地Buffer
        plab->set_word_size(buf_size);
        plab->set_buf(buf_space);
        record_survivor_plab(buf_space, buf_size);
        obj = plab->allocate(word_sz);
        // Note that we cannot compare buf_size < word_sz below
        // because of AlignmentReserve (see ParGCAllocBuffer::allocate()).
        assert(obj != NULL || plab->words_remaining() < word_sz,
               "Else should have been able to allocate");
        // It's conceivable that we may be able to use the
        // buffer we just grabbed for subsequent small requests
        // even if not for this one.
      } else {
        // We're used up.
        _to_space_full = true;
      }

    } else {	//直接从年青代的To区分配
      // Too large; allocate the object individually.
      obj = sp->par_allocate(word_sz);
    }
  }

  return obj;
}

/**
 * 回滚刚才的内存分配
 * 	1.如果对象是在Gc线程的本地Buffer中分配,则回收
 * 	2.如果对象是在To区直接分配,则放弃回收以填充代之
 */
void ParScanThreadState::undo_alloc_in_to_space(HeapWord* obj,
                                                size_t word_sz) {
  //内存块是在当前Gc线程的内存Buffer中分配,则回收
  if (to_space_alloc_buffer()->contains(obj)) {
    assert(to_space_alloc_buffer()->contains(obj + word_sz - 1),
           "Should contain whole object.");
    to_space_alloc_buffer()->undo_allocation(obj, word_sz);
  } else {	//内存块是在To区直接分配,则放弃回收以填充代之
    CollectedHeap::fill_with_object(obj, word_sz);
  }
}

void ParScanThreadState::print_and_clear_promotion_failure_size() {
  if (_promotion_failure_size != 0) {
    if (PrintPromotionFailure) {
      gclog_or_tty->print(" (%d: promotion failure size = " SIZE_FORMAT ") ",
        _thread_num, _promotion_failure_size);
    }
    _promotion_failure_size = 0;
  }
}

/**
 * 并行Gc线程状态集
 */
class ParScanThreadStateSet: private ResourceArray {
public:
  // Initializes states for the specified number of threads;
  ParScanThreadStateSet(int                     num_threads,
                        Space&                  to_space,
                        ParNewGeneration&       gen,
                        Generation&             old_gen,
                        ObjToScanQueueSet&      queue_set,
                        Stack<oop>*             overflow_stacks_,
                        size_t                  desired_plab_sz,
                        ParallelTaskTerminator& term);

  ~ParScanThreadStateSet() { TASKQUEUE_STATS_ONLY(reset_stats()); }

  inline ParScanThreadState& thread_state(int i);

  void reset(int active_workers, bool promotion_failed);
  void flush();

  #if TASKQUEUE_STATS
  static void
    print_termination_stats_hdr(outputStream* const st = gclog_or_tty);
  void print_termination_stats(outputStream* const st = gclog_or_tty);
  static void
    print_taskqueue_stats_hdr(outputStream* const st = gclog_or_tty);
  void print_taskqueue_stats(outputStream* const st = gclog_or_tty);
  void reset_stats();
  #endif // TASKQUEUE_STATS

private:
  ParallelTaskTerminator& _term;	//
  ParNewGeneration&       _gen;     //年青代内存管理器
  Generation&             _next_gen; //旧生代内存管理器

 public:
  bool is_valid(int id) const { return id < length(); }
  ParallelTaskTerminator* terminator() { return &_term; }
};


ParScanThreadStateSet::ParScanThreadStateSet(
  int num_threads, Space& to_space, ParNewGeneration& gen,
  Generation& old_gen, ObjToScanQueueSet& queue_set,
  Stack<oop>* overflow_stacks,
  size_t desired_plab_sz, ParallelTaskTerminator& term)
  : ResourceArray(sizeof(ParScanThreadState), num_threads),
    _gen(gen), _next_gen(old_gen), _term(term)
{
  //确保至少有一个Gc线程
  assert(num_threads > 0, "sanity check!");

  assert(ParGCUseLocalOverflow == (overflow_stacks != NULL), "overflow_stack allocation mismatch");

  //创建每一个Gc线程的状态管理器
  for (int i = 0; i < num_threads; ++i) {
    new ((ParScanThreadState*)_data + i)
        ParScanThreadState(&to_space, &gen, &old_gen, i, &queue_set,
                           overflow_stacks, desired_plab_sz, term);
  }
}

/**
 * 获取某一个Gc线程的状态管理器
 */
inline ParScanThreadState& ParScanThreadStateSet::thread_state(int i)
{
  assert(i >= 0 && i < length(), "sanity check!");
  return ((ParScanThreadState*)_data)[i];
}


void ParScanThreadStateSet::reset(int active_threads, bool promotion_failed)
{
  _term.reset_for_reuse(active_threads);
  if (promotion_failed) {
    for (int i = 0; i < length(); ++i) {
      thread_state(i).print_and_clear_promotion_failure_size();
    }
  }
}

#if TASKQUEUE_STATS
void
ParScanThreadState::reset_stats()
{
  taskqueue_stats().reset();
  _term_attempts = 0;
  _overflow_refills = 0;
  _overflow_refill_objs = 0;
}

void ParScanThreadStateSet::reset_stats()
{
  for (int i = 0; i < length(); ++i) {
    thread_state(i).reset_stats();
  }
}

void
ParScanThreadStateSet::print_termination_stats_hdr(outputStream* const st)
{
  st->print_raw_cr("GC Termination Stats");
  st->print_raw_cr("     elapsed  --strong roots-- "
                   "-------termination-------");
  st->print_raw_cr("thr     ms        ms       %   "
                   "    ms       %   attempts");
  st->print_raw_cr("--- --------- --------- ------ "
                   "--------- ------ --------");
}

void ParScanThreadStateSet::print_termination_stats(outputStream* const st)
{
  print_termination_stats_hdr(st);

  for (int i = 0; i < length(); ++i) {
    const ParScanThreadState & pss = thread_state(i);
    const double elapsed_ms = pss.elapsed_time() * 1000.0;
    const double s_roots_ms = pss.strong_roots_time() * 1000.0;
    const double term_ms = pss.term_time() * 1000.0;
    st->print_cr("%3d %9.2f %9.2f %6.2f "
                 "%9.2f %6.2f " SIZE_FORMAT_W(8),
                 i, elapsed_ms, s_roots_ms, s_roots_ms * 100 / elapsed_ms,
                 term_ms, term_ms * 100 / elapsed_ms, pss.term_attempts());
  }
}

// Print stats related to work queue activity.
void ParScanThreadStateSet::print_taskqueue_stats_hdr(outputStream* const st)
{
  st->print_raw_cr("GC Task Stats");
  st->print_raw("thr "); TaskQueueStats::print_header(1, st); st->cr();
  st->print_raw("--- "); TaskQueueStats::print_header(2, st); st->cr();
}

void ParScanThreadStateSet::print_taskqueue_stats(outputStream* const st)
{
  print_taskqueue_stats_hdr(st);

  TaskQueueStats totals;
  for (int i = 0; i < length(); ++i) {
    const ParScanThreadState & pss = thread_state(i);
    const TaskQueueStats & stats = pss.taskqueue_stats();
    st->print("%3d ", i); stats.print(st); st->cr();
    totals += stats;

    if (pss.overflow_refills() > 0) {
      st->print_cr("    " SIZE_FORMAT_W(10) " overflow refills    "
                   SIZE_FORMAT_W(10) " overflow objects",
                   pss.overflow_refills(), pss.overflow_refill_objs());
    }
  }
  st->print("tot "); totals.print(st); st->cr();

  DEBUG_ONLY(totals.verify());
}
#endif // TASKQUEUE_STATS

void ParScanThreadStateSet::flush()
{
  // Work in this loop should be kept as lightweight as
  // possible since this might otherwise become a bottleneck
  // to scaling. Should we add heavy-weight work into this
  // loop, consider parallelizing the loop into the worker threads.
  for (int i = 0; i < length(); ++i) {
    ParScanThreadState& par_scan_state = thread_state(i);

    // Flush stats related to To-space PLAB activity and
    // retire the last buffer.
    par_scan_state.to_space_alloc_buffer()->
      flush_stats_and_retire(_gen.plab_stats(),
                             false /* !retain */);

    // Every thread has its own age table.  We need to merge
    // them all into one.
    ageTable *local_table = par_scan_state.age_table();
    _gen.age_table()->merge(local_table);

    // Inform old gen that we're done.
    _next_gen.par_promote_alloc_done(i);
    _next_gen.par_oop_since_save_marks_iterate_done(i);
  }

  if (UseConcMarkSweepGC && ParallelGCThreads > 0) {
    // We need to call this even when ResizeOldPLAB is disabled
    // so as to avoid breaking some asserts. While we may be able
    // to avoid this by reorganizing the code a bit, I am loathe
    // to do that unless we find cases where ergo leads to bad
    // performance.
    CFLS_LAB::compute_desired_plab_size();
  }
}

ParScanClosure::ParScanClosure(ParNewGeneration* g,
                               ParScanThreadState* par_scan_state) :
  OopsInGenClosure(g), _par_scan_state(par_scan_state), _g(g)
{
  assert(_g->level() == 0, "Optimized for youngest generation");
  _boundary = _g->reserved().end();
}

void ParScanWithBarrierClosure::do_oop(oop* p)       { ParScanClosure::do_oop_work(p, true, false); }
void ParScanWithBarrierClosure::do_oop(narrowOop* p) { ParScanClosure::do_oop_work(p, true, false); }

void ParScanWithoutBarrierClosure::do_oop(oop* p)       { ParScanClosure::do_oop_work(p, false, false); }
void ParScanWithoutBarrierClosure::do_oop(narrowOop* p) { ParScanClosure::do_oop_work(p, false, false); }

void ParRootScanWithBarrierTwoGensClosure::do_oop(oop* p)       { ParScanClosure::do_oop_work(p, true, true); }
void ParRootScanWithBarrierTwoGensClosure::do_oop(narrowOop* p) { ParScanClosure::do_oop_work(p, true, true); }

void ParRootScanWithoutBarrierClosure::do_oop(oop* p)       { ParScanClosure::do_oop_work(p, false, true); }
void ParRootScanWithoutBarrierClosure::do_oop(narrowOop* p) { ParScanClosure::do_oop_work(p, false, true); }

ParScanWeakRefClosure::ParScanWeakRefClosure(ParNewGeneration* g,
                                             ParScanThreadState* par_scan_state)
  : ScanWeakRefClosure(g), _par_scan_state(par_scan_state)
{}

void ParScanWeakRefClosure::do_oop(oop* p)       { ParScanWeakRefClosure::do_oop_work(p); }
void ParScanWeakRefClosure::do_oop(narrowOop* p) { ParScanWeakRefClosure::do_oop_work(p); }

#ifdef WIN32
#pragma warning(disable: 4786) /* identifier was truncated to '255' characters in the browser information */
#endif

ParEvacuateFollowersClosure::ParEvacuateFollowersClosure(
    ParScanThreadState* par_scan_state_,
    ParScanWithoutBarrierClosure* to_space_closure_,
    ParScanWithBarrierClosure* old_gen_closure_,
    ParRootScanWithoutBarrierClosure* to_space_root_closure_,
    ParNewGeneration* par_gen_,
    ParRootScanWithBarrierTwoGensClosure* old_gen_root_closure_,
    ObjToScanQueueSet* task_queues_,
    ParallelTaskTerminator* terminator_) :

    _par_scan_state(par_scan_state_),
    _to_space_closure(to_space_closure_),
    _old_gen_closure(old_gen_closure_),
    _to_space_root_closure(to_space_root_closure_),
    _old_gen_root_closure(old_gen_root_closure_),
    _par_gen(par_gen_),
    _task_queues(task_queues_),
    _terminator(terminator_)
{}

/**
 * 递归扫描复制对象的引用对象
 *
 * 扫描复制任务(对象)的来源
 *   1.当前Gc线程的工作队列
 *   2.偷取其它Gc线程工作队列中的对象
 *   3.当前Gc线程的临时溢出栈
 */
void ParEvacuateFollowersClosure::do_void() {

  //获取当前Gc线程的工作队列
  ObjToScanQueue* work_q = par_scan_state()->work_queue();

  while (true) {

    // Scan to-space and old-gen objs until we run out of both.
    oop obj_to_scan;
    //处理当前Gc线程工作队列中的所有对象
    par_scan_state()->trim_queues(0);

    //偷取其它Gc线程工作队列中的对象
    if (task_queues()->steal(par_scan_state()->thread_num(),
                             par_scan_state()->hash_seed(),
                             obj_to_scan)) {
      bool res = work_q->push(obj_to_scan);
      assert(res, "Empty queue should have room for a push.");

      //   if successful, goto Start.
      continue;

      // try global overflow list.
    } else if (par_gen()->take_from_overflow_list(par_scan_state())) {
      continue;
    }

    // Otherwise, offer termination.
    par_scan_state()->start_term_time();
    if (terminator()->offer_termination()) break;
    par_scan_state()->end_term_time();
  }

  assert(par_gen()->_overflow_list == NULL && par_gen()->_num_par_pushes == 0,
         "Broken overflow list?");
  // Finish the last termination pause.
  par_scan_state()->end_term_time();
}

ParNewGenTask::ParNewGenTask(ParNewGeneration* gen, Generation* next_gen,
                HeapWord* young_old_boundary, ParScanThreadStateSet* state_set) :
    AbstractGangTask("ParNewGeneration collection"),
    _gen(gen), _next_gen(next_gen),
    _young_old_boundary(young_old_boundary),
    _state_set(state_set)
  {}

// Reset the terminator for the given number of
// active threads.
void ParNewGenTask::set_for_termination(int active_workers) {
  _state_set->reset(active_workers, _gen->promotion_failed());

  // Should the heap be passed in?  There's only 1 for now so
  // grab it instead.
  GenCollectedHeap* gch = GenCollectedHeap::heap();
  gch->set_n_termination(active_workers);
}

// The "i" passed to this method is the part of the work for
// this thread.  It is not the worker ID.  The "i" is derived
// from _started_workers which is incremented in internal_note_start()
// called in GangWorker loop() and which is called under the
// which is  called under the protection of the gang monitor and is
// called after a task is started.  So "i" is based on
// first-come-first-served.

/**
 * Gc线程开始回收新生代的垃圾对象
 */
void ParNewGenTask::work(uint worker_id) {
  GenCollectedHeap* gch = GenCollectedHeap::heap();
  // Since this is being done in a separate thread, need new resource
  // and handle marks.
  ResourceMark rm;
  HandleMark hm;

  //
  assert(gch->n_gens() == 2, "Par young collection currently only works with one older gen.");

  //旧生代内存管理器
  Generation* old_gen = gch->next_gen(_gen);

  //获取当前Gc线程的私有数据
  ParScanThreadState& par_scan_state = _state_set->thread_state(worker_id);
  assert(_state_set->is_valid(worker_id), "Should not have been called");

  par_scan_state.set_young_old_boundary(_young_old_boundary);

  par_scan_state.start_strong_roots();	//标记开始时间

  //开始扫描复制所有的根对象(active对象的初始集)
  gch->gen_process_strong_roots(_gen->level(),
                                true,  // Process younger gens, if any,
                                       // as strong roots.
                                false, // no scope; this is parallel code
                                false, // not collecting perm generation.
                                SharedHeap::SO_AllClasses,
                                &par_scan_state.to_space_root_closure(),
                                true,   // walk *all* scavengable nmethods
                                &par_scan_state.older_gen_closure());

  par_scan_state.end_strong_roots();	//标记结束时间

  //开始迭代扫描复制所有active对象的引用对象
  par_scan_state.evacuate_followers_closure().do_void();
}

#ifdef _MSC_VER
#pragma warning( push )
#pragma warning( disable:4355 ) // 'this' : used in base member initializer list
#endif

ParNewGeneration::ParNewGeneration(ReservedSpace rs, size_t initial_byte_size, int level)
  : DefNewGeneration(rs, initial_byte_size, level, "PCopy"),
  _overflow_list(NULL),
  _is_alive_closure(this),
  _plab_stats(YoungPLABSize, PLABWeight)
{
  NOT_PRODUCT(_overflow_counter = ParGCWorkQueueOverflowInterval;)
  NOT_PRODUCT(_num_par_pushes = 0;)

  _task_queues = new ObjToScanQueueSet(ParallelGCThreads);

  guarantee(_task_queues != NULL, "task_queues allocation failure.");

  printf("%s[%d] [tid: %lu]: 开始创建 %u个任务队列.\n", __FILE__, __LINE__, pthread_self(), ParallelGCThreads);

  for (uint i1 = 0; i1 < ParallelGCThreads; i1++) {
    ObjToScanQueue *q = new ObjToScanQueue();
    guarantee(q != NULL, "work_queue Allocation failure.");
    _task_queues->register_queue(i1, q);
  }

  for (uint i2 = 0; i2 < ParallelGCThreads; i2++)
    _task_queues->queue(i2)->initialize();

  _overflow_stacks = NULL;
  if (ParGCUseLocalOverflow) {
    _overflow_stacks = NEW_C_HEAP_ARRAY(Stack<oop>, ParallelGCThreads);
    for (size_t i = 0; i < ParallelGCThreads; ++i) {
      new (_overflow_stacks + i) Stack<oop>();
    }
  }

  if (UsePerfData) {
    EXCEPTION_MARK;
    ResourceMark rm;

    const char* cname = PerfDataManager::counter_name(_gen_counters->name_space(), "threads");
    PerfDataManager::create_constant(SUN_GC, cname, PerfData::U_None, ParallelGCThreads, CHECK);
  }
}
#ifdef _MSC_VER
#pragma warning( pop )
#endif

// ParNewGeneration::
ParKeepAliveClosure::ParKeepAliveClosure(ParScanWeakRefClosure* cl) :
  DefNewGeneration::KeepAliveClosure(cl), _par_cl(cl) {}

template <class T>
void /*ParNewGeneration::*/ParKeepAliveClosure::do_oop_work(T* p) {
#ifdef ASSERT
  {
    assert(!oopDesc::is_null(*p), "expected non-null ref");
    oop obj = oopDesc::load_decode_heap_oop_not_null(p);
    // We never expect to see a null reference being processed
    // as a weak reference.
    assert(obj->is_oop(), "expected an oop while scanning weak refs");
  }
#endif // ASSERT

  _par_cl->do_oop_nv(p);

  if (Universe::heap()->is_in_reserved(p)) {
    oop obj = oopDesc::load_decode_heap_oop_not_null(p);
    _rs->write_ref_field_gc_par(p, obj);
  }
}

void /*ParNewGeneration::*/ParKeepAliveClosure::do_oop(oop* p)       { ParKeepAliveClosure::do_oop_work(p); }
void /*ParNewGeneration::*/ParKeepAliveClosure::do_oop(narrowOop* p) { ParKeepAliveClosure::do_oop_work(p); }

// ParNewGeneration::
KeepAliveClosure::KeepAliveClosure(ScanWeakRefClosure* cl) :
  DefNewGeneration::KeepAliveClosure(cl) {}

template <class T>
void /*ParNewGeneration::*/KeepAliveClosure::do_oop_work(T* p) {
#ifdef ASSERT
  {
    assert(!oopDesc::is_null(*p), "expected non-null ref");
    oop obj = oopDesc::load_decode_heap_oop_not_null(p);
    // We never expect to see a null reference being processed
    // as a weak reference.
    assert(obj->is_oop(), "expected an oop while scanning weak refs");
  }
#endif // ASSERT

  _cl->do_oop_nv(p);

  if (Universe::heap()->is_in_reserved(p)) {
    oop obj = oopDesc::load_decode_heap_oop_not_null(p);
    _rs->write_ref_field_gc_par(p, obj);
  }
}

void /*ParNewGeneration::*/KeepAliveClosure::do_oop(oop* p)       { KeepAliveClosure::do_oop_work(p); }
void /*ParNewGeneration::*/KeepAliveClosure::do_oop(narrowOop* p) { KeepAliveClosure::do_oop_work(p); }

template <class T> void ScanClosureWithParBarrier::do_oop_work(T* p) {
  T heap_oop = oopDesc::load_heap_oop(p);
  if (!oopDesc::is_null(heap_oop)) {
    oop obj = oopDesc::decode_heap_oop_not_null(heap_oop);
    if ((HeapWord*)obj < _boundary) {
      assert(!_g->to()->is_in_reserved(obj), "Scanning field twice?");
      oop new_obj = obj->is_forwarded()
                      ? obj->forwardee()
                      : _g->DefNewGeneration::copy_to_survivor_space(obj);
      oopDesc::encode_store_heap_oop_not_null(p, new_obj);
    }
    if (_gc_barrier) {
      // If p points to a younger generation, mark the card.
      if ((HeapWord*)obj < _gen_boundary) {
        _rs->write_ref_field_gc_par(p, obj);
      }
    }
  }
}

void ScanClosureWithParBarrier::do_oop(oop* p)       { ScanClosureWithParBarrier::do_oop_work(p); }
void ScanClosureWithParBarrier::do_oop(narrowOop* p) { ScanClosureWithParBarrier::do_oop_work(p); }

class ParNewRefProcTaskProxy: public AbstractGangTask {
  typedef AbstractRefProcTaskExecutor::ProcessTask ProcessTask;
public:
  ParNewRefProcTaskProxy(ProcessTask& task, ParNewGeneration& gen,
                         Generation& next_gen,
                         HeapWord* young_old_boundary,
                         ParScanThreadStateSet& state_set);

private:
  virtual void work(uint worker_id);
  virtual void set_for_termination(int active_workers) {
    _state_set.terminator()->reset_for_reuse(active_workers);
  }
private:
  ParNewGeneration&      _gen;
  ProcessTask&           _task;
  Generation&            _next_gen;
  HeapWord*              _young_old_boundary;
  ParScanThreadStateSet& _state_set;
};

ParNewRefProcTaskProxy::ParNewRefProcTaskProxy(
    ProcessTask& task, ParNewGeneration& gen,
    Generation& next_gen,
    HeapWord* young_old_boundary,
    ParScanThreadStateSet& state_set)
  : AbstractGangTask("ParNewGeneration parallel reference processing"),
    _gen(gen),
    _task(task),
    _next_gen(next_gen),
    _young_old_boundary(young_old_boundary),
    _state_set(state_set)
{
}

void ParNewRefProcTaskProxy::work(uint worker_id)
{
  ResourceMark rm;
  HandleMark hm;
  ParScanThreadState& par_scan_state = _state_set.thread_state(worker_id);
  par_scan_state.set_young_old_boundary(_young_old_boundary);
  _task.work(worker_id, par_scan_state.is_alive_closure(),
             par_scan_state.keep_alive_closure(),
             par_scan_state.evacuate_followers_closure());
}

class ParNewRefEnqueueTaskProxy: public AbstractGangTask {
  typedef AbstractRefProcTaskExecutor::EnqueueTask EnqueueTask;
  EnqueueTask& _task;

public:
  ParNewRefEnqueueTaskProxy(EnqueueTask& task)
    : AbstractGangTask("ParNewGeneration parallel reference enqueue"),
      _task(task)
  { }

  virtual void work(uint worker_id)
  {
    _task.work(worker_id);
  }
};


void ParNewRefProcTaskExecutor::execute(ProcessTask& task)
{
  GenCollectedHeap* gch = GenCollectedHeap::heap();
  assert(gch->kind() == CollectedHeap::GenCollectedHeap,
         "not a generational heap");
  FlexibleWorkGang* workers = gch->workers();
  assert(workers != NULL, "Need parallel worker threads.");
  _state_set.reset(workers->active_workers(), _generation.promotion_failed());
  ParNewRefProcTaskProxy rp_task(task, _generation, *_generation.next_gen(),
                                 _generation.reserved().end(), _state_set);
  workers->run_task(&rp_task);
  _state_set.reset(0 /* bad value in debug if not reset */,
                   _generation.promotion_failed());
}

void ParNewRefProcTaskExecutor::execute(EnqueueTask& task)
{
  GenCollectedHeap* gch = GenCollectedHeap::heap();
  FlexibleWorkGang* workers = gch->workers();
  assert(workers != NULL, "Need parallel worker threads.");
  ParNewRefEnqueueTaskProxy enq_task(task);
  workers->run_task(&enq_task);
}

void ParNewRefProcTaskExecutor::set_single_threaded_mode()
{
  _state_set.flush();
  GenCollectedHeap* gch = GenCollectedHeap::heap();
  gch->set_par_threads(0);  // 0 ==> non-parallel.
  gch->save_marks();
}

ScanClosureWithParBarrier::
ScanClosureWithParBarrier(ParNewGeneration* g, bool gc_barrier) :
  ScanClosure(g, gc_barrier) {}

EvacuateFollowersClosureGeneral::
EvacuateFollowersClosureGeneral(GenCollectedHeap* gch, int level,
                                OopsInGenClosure* cur,
                                OopsInGenClosure* older) :
  _gch(gch), _level(level),
  _scan_cur_or_nonheap(cur), _scan_older(older)
{}

void EvacuateFollowersClosureGeneral::do_void() {
  do {
    // Beware: this call will lead to closure applications via virtual
    // calls.
    _gch->oop_since_save_marks_iterate(_level,
                                       _scan_cur_or_nonheap,
                                       _scan_older);
  } while (!_gch->no_allocs_since_save_marks(_level));
}


bool ParNewGeneration::_avoid_promotion_undo = false;

void ParNewGeneration::adjust_desired_tenuring_threshold() {
  // Set the desired survivor size to half the real survivor space
  _tenuring_threshold =
    age_table()->compute_tenuring_threshold(to()->capacity()/HeapWordSize);
}

/**
 * 回收当前内存代中的垃圾对象
 */
void ParNewGeneration::collect(bool   full,
                               bool   clear_all_soft_refs,
                               size_t size,
                               bool   is_tlab) {
  assert(full || size > 0, "otherwise we don't want to collect");

  printf("%s[%d] [tid: %lu]: 开始对年青代[%s]进行垃圾回收...\n", __FILE__, __LINE__, pthread_self(), this->name());

  GenCollectedHeap* gch = GenCollectedHeap::heap();

  //确保当前JVM的内存堆管理器是基于分代管理的内存堆管理器
  assert(gch->kind() == CollectedHeap::GenCollectedHeap, "not a CMS generational heap");

  AdaptiveSizePolicy* size_policy = gch->gen_policy()->size_policy();
  //Gc线程管理器
  FlexibleWorkGang* workers = gch->workers();
  assert(workers != NULL, "Need workgang for parallel work");

  //调整本次年青代垃圾回收所需的Gc工作线程数量
  int active_workers =  AdaptiveSizePolicy::calc_active_workers(workers->total_workers(),
                                   workers->active_workers(),
                                   Threads::number_of_non_daemon_threads());

  printf("%s[%d] [tid: %lu]: 年青代[%s]的垃圾回收将使用%d个工作线程来进行...\n", __FILE__, __LINE__, pthread_self(), this->name(), active_workers);

  workers->set_active_workers(active_workers);

  //获取旧生代内存管理器
  _next_gen = gch->next_gen(this);
  assert(_next_gen != NULL, "This must be the youngest gen, and not the only gen");
  assert(gch->n_gens() == 2, "Par collection currently only works with single older gen.");

  //如果旧生代配置的CMS,则不支持对象的升级回滚
  if (gch->collector_policy()->is_concurrent_mark_sweep_policy()) {
    set_avoid_promotion_undo(true);
  }

  // If the next generation is too full to accomodate worst-case promotion
  // from this generation, pass on collection; let the next generation
  // do it.
  if (!collection_attempt_is_safe()) {//当前无法对内存代进行Gc
    gch->set_incremental_collection_failed();  // slight lie, in that we did not even attempt one
    return;
  }

  //确保To区完全空闲
  assert(to()->is_empty(), "Else not collection_attempt_is_safe");

  init_assuming_no_promotion_failure();

  if (UseAdaptiveSizePolicy) {
    set_survivor_overflow(false);
    size_policy->minor_collection_begin();
  }

  TraceTime t1("GC", PrintGC && !PrintGCDetails, true, gclog_or_tty);

  //当前整个内存堆的使用量
  size_t gch_prev_used = gch->used();

  SpecializationStats::clear();

  age_table()->clear();
  to()->clear(SpaceDecorator::Mangle);

  gch->save_marks();
  assert(workers != NULL, "Need parallel worker threads.");
  int n_workers = active_workers;

  // Set the correct parallelism (number of queues) in the reference processor
  ref_processor()->set_active_mt_degree(n_workers);

  // Always set the terminator for the active number of workers
  // because only those workers go through the termination protocol.
  ParallelTaskTerminator _term(n_workers, task_queues());
  ParScanThreadStateSet thread_state_set(workers->active_workers(),
                                         *to(), *this, *_next_gen, *task_queues(),
                                         _overflow_stacks, desired_plab_sz(), _term);

  ParNewGenTask tsk(this, _next_gen, reserved().end(), &thread_state_set);
  gch->set_par_threads(n_workers);
  gch->rem_set()->prepare_for_younger_refs_iterate(true);

  // It turns out that even when we're using 1 thread, doing the work in a
  // separate thread causes wide variance in run times.  We can't help this
  // in the multi-threaded case, but we special-case n=1 here to get
  // repeatable measurements of the 1-thread overhead of the parallel code.
  if (n_workers > 1) {	//多线程并发执行Gc
    GenCollectedHeap::StrongRootsScope srs(gch);
    workers->run_task(&tsk);
  } else {
    GenCollectedHeap::StrongRootsScope srs(gch);
    tsk.work(0);
  }

  thread_state_set.reset(0 /* Bad value in debug if not reset */, promotion_failed());

  // Process (weak) reference objects found during scavenge.
  ReferenceProcessor* rp = ref_processor();
  IsAliveClosure is_alive(this);
  ScanWeakRefClosure scan_weak_ref(this);
  KeepAliveClosure keep_alive(&scan_weak_ref);
  ScanClosure               scan_without_gc_barrier(this, false);
  ScanClosureWithParBarrier scan_with_gc_barrier(this, true);
  set_promo_failure_scan_stack_closure(&scan_without_gc_barrier);
  EvacuateFollowersClosureGeneral evacuate_followers(gch, _level, &scan_without_gc_barrier, &scan_with_gc_barrier);
  rp->setup_policy(clear_all_soft_refs);
  // Can  the mt_degree be set later (at run_task() time would be best)?
  rp->set_active_mt_degree(active_workers);
  if (rp->processing_is_mt()) {
    ParNewRefProcTaskExecutor task_executor(*this, thread_state_set);
    rp->process_discovered_references(&is_alive, &keep_alive,
                                      &evacuate_followers, &task_executor);
  } else {
    thread_state_set.flush();
    gch->set_par_threads(0);  // 0 ==> non-parallel.
    gch->save_marks();
    rp->process_discovered_references(&is_alive, &keep_alive,
                                      &evacuate_followers, NULL);
  }


  if (!promotion_failed()) {
    // Swap the survivor spaces.
    eden()->clear(SpaceDecorator::Mangle);
    from()->clear(SpaceDecorator::Mangle);
    if (ZapUnusedHeapArea) {
      // This is now done here because of the piece-meal mangling which
      // can check for valid mangling at intermediate points in the
      // collection(s).  When a minor collection fails to collect
      // sufficient space resizing of the young generation can occur
      // an redistribute the spaces in the young generation.  Mangle
      // here so that unzapped regions don't get distributed to
      // other spaces.
      to()->mangle_unused_area();
    }
    swap_spaces();

    // A successful scavenge should restart the GC time limit count which is
    // for full GC's.
    size_policy->reset_gc_overhead_limit_count();

    assert(to()->is_empty(), "to space should be empty now");
  } else {
    assert(_promo_failure_scan_stack.is_empty(), "post condition");
    _promo_failure_scan_stack.clear(true); // Clear cached segments.

    remove_forwarding_pointers();
    if (PrintGCDetails) {
      gclog_or_tty->print(" (promotion failed)");
    }

    // All the spaces are in play for mark-sweep.
    swap_spaces();  // Make life simpler for CMS || rescan; see 6483690.
    from()->set_next_compaction_space(to());
    gch->set_incremental_collection_failed();
    // Inform the next generation that a promotion failure occurred.
    _next_gen->promotion_failure_occurred();

    // Reset the PromotionFailureALot counters.
    NOT_PRODUCT(Universe::heap()->reset_promotion_should_fail();)
  }

  // set new iteration safe limit for the survivor spaces
  from()->set_concurrent_iteration_safe_limit(from()->top());
  to()->set_concurrent_iteration_safe_limit(to()->top());

  adjust_desired_tenuring_threshold();
  if (ResizePLAB) {
    plab_stats()->adjust_desired_plab_sz();
  }

  if (PrintGC && !PrintGCDetails) {
    gch->print_heap_change(gch_prev_used);
  }

  if (PrintGCDetails && ParallelGCVerbose) {
    TASKQUEUE_STATS_ONLY(thread_state_set.print_termination_stats());
    TASKQUEUE_STATS_ONLY(thread_state_set.print_taskqueue_stats());
  }

  if (UseAdaptiveSizePolicy) {
    size_policy->minor_collection_end(gch->gc_cause());
    size_policy->avg_survived()->sample(from()->used());
  }

  // We need to use a monotonically non-deccreasing time in ms
  // or we will see time-warp warnings and os::javaTimeMillis()
  // does not guarantee monotonicity.
  jlong now = os::javaTimeNanos() / NANOSECS_PER_MILLISEC;
  update_time_of_last_gc(now);

  SpecializationStats::print();

  rp->set_enqueuing_is_done(true);
  if (rp->processing_is_mt()) {
    ParNewRefProcTaskExecutor task_executor(*this, thread_state_set);
    rp->enqueue_discovered_references(&task_executor);
  } else {
    rp->enqueue_discovered_references(NULL);
  }
  rp->verify_no_references_recorded();
}

static int sum;
void ParNewGeneration::waste_some_time() {
  for (int i = 0; i < 100; i++) {
    sum += i;
  }
}

static const oop ClaimedForwardPtr = oop(0x4);

// Because of concurrency, there are times where an object for which
// "is_forwarded()" is true contains an "interim" forwarding pointer
// value.  Such a value will soon be overwritten with a real value.
// This method requires "obj" to have a forwarding pointer, and waits, if
// necessary for a real one to be inserted, and returns it.

/**
 * 解析对象的物理存储地址
 */
oop ParNewGeneration::real_forwardee(oop obj) {
  oop forward_ptr = obj->forwardee();
  if (forward_ptr != ClaimedForwardPtr) {	//对象已被其它Gc线程处理完
    return forward_ptr;
  } else {	//对象正在被其它Gc线程处理
    return real_forwardee_slow(obj);
  }
}

/**
 * 当前Gc线程等待指定的对象被其它Gc线程处理完
 */
oop ParNewGeneration::real_forwardee_slow(oop obj) {
  // Spin-read if it is claimed but not yet written by another thread.
  oop forward_ptr = obj->forwardee();
  while (forward_ptr == ClaimedForwardPtr) {
    waste_some_time();
    assert(obj->is_forwarded(), "precondition");
    forward_ptr = obj->forwardee();
  }
  return forward_ptr;
}

#ifdef ASSERT
bool ParNewGeneration::is_legal_forward_ptr(oop p) {
  return
    (_avoid_promotion_undo && p == ClaimedForwardPtr)
    || Universe::heap()->is_in_reserved(p);
}
#endif

void ParNewGeneration::preserve_mark_if_necessary(oop obj, markOop m) {
  if (m->must_be_preserved_for_promotion_failure(obj)) {
    // We should really have separate per-worker stacks, rather
    // than use locking of a common pair of stacks.
    MutexLocker ml(ParGCRareEvent_lock);
    preserve_mark(obj, m);
  }
}

// Multiple GC threads may try to promote an object.  If the object
// is successfully promoted, a forwarding pointer will be installed in
// the object in the young generation.  This method claims the right
// to install the forwarding pointer before it copies the object,
// thus avoiding the need to undo the copy as in
// copy_to_survivor_space_avoiding_with_undo.
/**
 * 复制指定的对象到To区(幸存区)
 * 对象升级策略(存活很长时间的对象转存储到年老代):
 *    1.更新老对象的物理存储空间地址
 *    2.从年老代中为老对象分配存储空间
 */
oop ParNewGeneration::copy_to_survivor_space_avoiding_promotion_undo(
        ParScanThreadState* par_scan_state, oop old, size_t sz, markOop m) {
  // In the sequential version, this assert also says that the object is
  // not forwarded.  That might not be the case here.  It is the case that
  // the caller observed it to be not forwarded at some time in the past.
  assert(is_in_reserved(old), "shouldn't be scavenging this oop");

  // The sequential code read "old->age()" below.  That doesn't work here,
  // since the age is in the mark word, and that might be overwritten with
  // a forwarding pointer by a parallel thread.  So we must save the mark
  // word in a local and then analyze it.
  oopDesc dummyOld;
  dummyOld.set_mark(m);
  assert(!dummyOld.is_forwarded(),
         "should not be called with forwarding pointer mark word.");

  oop new_obj = NULL;
  oop forward_ptr;

  // Try allocating obj in to-space (unless too old)
  //对象年龄不够则复制到To区
  if (dummyOld.age() < tenuring_threshold()) {
	//在To区中为待复制对象申请存储空间
    new_obj = (oop)par_scan_state->alloc_in_to_space(sz);
    if (new_obj == NULL) {
      set_survivor_overflow(true);
    }
  }

  if (new_obj == NULL) {	//To区分配失败或对象的年龄可以升级到下一个内存代

    // Attempt to install a null forwarding pointer (atomically),
    // to claim the right to install the real forwarding pointer.
	//试图将待复制对象的存储空间指针置空
    forward_ptr = old->forward_to_atomic(ClaimedForwardPtr);
    if (forward_ptr != NULL) {	//该对象正在被其它垃圾回收线程处理或已处理完了
      // someone else beat us to it.
        return real_forwardee(old);
    }

    //从下一个内存代中申请内存空间来存储待复制的对象
    new_obj = _next_gen->par_promote(par_scan_state->thread_num(), old, m, sz);

    //转存储失败(对象升级)
    if (new_obj == NULL) {
      // promotion failed, forward to self
      _promotion_failed = true;
      new_obj = old;

      preserve_mark_if_necessary(old, m);
      // Log the size of the maiden promotion failure
      par_scan_state->log_promotion_failure(sz);
    }

    //更新旧对象的存储空间指针
    old->forward_to(new_obj);
    forward_ptr = NULL;
  } else {
	//复制旧对象到新的存储空间
    Copy::aligned_disjoint_words((HeapWord*)old, (HeapWord*)new_obj, sz);

    //更新旧对象的存储空间指针
    forward_ptr = old->forward_to_atomic(new_obj);

    //复制旧对象的标记信息到新对象
    new_obj->set_mark(m);
    // Increment age if obj still in new generation
    new_obj->incr_age();

    par_scan_state->age_table()->add(new_obj, sz);
  }
  assert(new_obj != NULL, "just checking");

  /**
   * 将刚刚处理完的对象放入当前Gc线程的工作队列中,以便后续扫描复制其所有的引用对象(属性),
   * 如果工作队列满,则先暂存到溢出栈中
   */
  if (forward_ptr == NULL) {	//旧对象被当前Gc线程处理
    oop obj_to_push = new_obj;

    if (par_scan_state->should_be_partially_scanned(obj_to_push, old)) {
      // Length field used as index of next element to be scanned.
      // Real length can be obtained from real_forwardee()
      arrayOop(old)->set_length(0);
      obj_to_push = old;
      assert(obj_to_push->is_forwarded() && obj_to_push->forwardee() != obj_to_push,
             "push forwarded object");
    }

    // Push it on one of the queues of to-be-scanned objects.
    bool simulate_overflow = false;
    NOT_PRODUCT(
      if (ParGCWorkQueueOverflowALot && should_simulate_overflow()) {
        // simulate a stack overflow
        simulate_overflow = true;
      }
    )

    if (simulate_overflow || !par_scan_state->work_queue()->push(obj_to_push)) {
      // Add stats for overflow pushes.
      if (Verbose && PrintGCDetails) {
        gclog_or_tty->print("queue overflow!\n");
      }
      push_on_overflow_list(old, par_scan_state);
      TASKQUEUE_STATS_ONLY(par_scan_state->taskqueue_stats().record_overflow(0));
    }

    return new_obj;
  }

  //旧对象被当其它垃圾回收线程抢先处理了，则回滚新分配的内存空间
  if (is_in_reserved(new_obj)) {
    // Must be in to_space.
    assert(to()->is_in_reserved(new_obj), "Checking");
    if (forward_ptr == ClaimedForwardPtr) {
      // Wait to get the real forwarding pointer value.
      forward_ptr = real_forwardee(old);
    }
    par_scan_state->undo_alloc_in_to_space((HeapWord*)new_obj, sz);
  }

  return forward_ptr;
}


// Multiple GC threads may try to promote the same object.  If two
// or more GC threads copy the object, only one wins the race to install
// the forwarding pointer.  The other threads have to undo their copy.
/**
 * 复制指定的对象到To区(幸存区)
 * 对象升级策略(存活很长时间的对象转存储到年老代):
 *    1.从年老代中为老对象分配存储空间
 *    2.更新老对象的物理存储空间地址
 */
oop ParNewGeneration::copy_to_survivor_space_with_undo(
        ParScanThreadState* par_scan_state, oop old, size_t sz, markOop m) {

  // In the sequential version, this assert also says that the object is
  // not forwarded.  That might not be the case here.  It is the case that
  // the caller observed it to be not forwarded at some time in the past.
  assert(is_in_reserved(old), "shouldn't be scavenging this oop");

  // The sequential code read "old->age()" below.  That doesn't work here,
  // since the age is in the mark word, and that might be overwritten with
  // a forwarding pointer by a parallel thread.  So we must save the mark
  // word here, install it in a local oopDesc, and then analyze it.
  oopDesc dummyOld;
  dummyOld.set_mark(m);
  assert(!dummyOld.is_forwarded(), "should not be called with forwarding pointer mark word.");

  bool failed_to_promote = false;
  oop new_obj = NULL;
  oop forward_ptr;

  //对象年龄不够则复制到To区
  if (dummyOld.age() < tenuring_threshold()) {
    new_obj = (oop)par_scan_state->alloc_in_to_space(sz);
    if (new_obj == NULL) {
      set_survivor_overflow(true);
    }
  }

  if (new_obj == NULL) {	//To区分配失败或对象的年龄可以升级到下一个内存代
	//从下一个内存代中申请内存空间来存储待复制的对象
    new_obj = _next_gen->par_promote(par_scan_state->thread_num(), old, m, sz);

    if (new_obj == NULL) {	//对象升级/转存储失败
      // promotion failed, forward to self
      forward_ptr = old->forward_to_atomic(old);
      new_obj = old;

      if (forward_ptr != NULL) {
        return forward_ptr;   // someone else succeeded
      }

      _promotion_failed = true;
      failed_to_promote = true;

      preserve_mark_if_necessary(old, m);
      // Log the size of the maiden promotion failure
      par_scan_state->log_promotion_failure(sz);
    }
  } else {
    // Is in to-space; do copying ourselves.
    Copy::aligned_disjoint_words((HeapWord*)old, (HeapWord*)new_obj, sz);
    // Restore the mark word copied above.
    new_obj->set_mark(m);
    // Increment age if new_obj still in new generation
    new_obj->incr_age();
    par_scan_state->age_table()->add(new_obj, sz);
  }
  assert(new_obj != NULL, "just checking");

  //更新老对象的物理地址空间
  if (!failed_to_promote) {
    forward_ptr = old->forward_to_atomic(new_obj);
  }

  if (forward_ptr == NULL) {	//当前垃圾回收线程强先更新老对象的物理地址空间
    oop obj_to_push = new_obj;
    if (par_scan_state->should_be_partially_scanned(obj_to_push, old)) {
      // Length field used as index of next element to be scanned.
      // Real length can be obtained from real_forwardee()
      arrayOop(old)->set_length(0);
      obj_to_push = old;
      assert(obj_to_push->is_forwarded() && obj_to_push->forwardee() != obj_to_push,
             "push forwarded object");
    }
    // Push it on one of the queues of to-be-scanned objects.
    bool simulate_overflow = false;
    NOT_PRODUCT(
      if (ParGCWorkQueueOverflowALot && should_simulate_overflow()) {
        // simulate a stack overflow
        simulate_overflow = true;
      }
    )

    if (simulate_overflow || !par_scan_state->work_queue()->push(obj_to_push)) {
      // Add stats for overflow pushes.
      push_on_overflow_list(old, par_scan_state);
      TASKQUEUE_STATS_ONLY(par_scan_state->taskqueue_stats().record_overflow(0));
    }

    return new_obj;
  }

  //老对象的物理地址空间被其它线程抢先更新,则回收分配的存储空间
  if (is_in_reserved(new_obj)) {
    // Must be in to_space.
    assert(to()->is_in_reserved(new_obj), "Checking");
    par_scan_state->undo_alloc_in_to_space((HeapWord*)new_obj, sz);
  } else {
    assert(!_avoid_promotion_undo, "Should not be here if avoiding.");
    _next_gen->par_promote_alloc_undo(par_scan_state->thread_num(),
                                      (HeapWord*)new_obj, sz);
  }

  return forward_ptr;
}

#ifndef PRODUCT
// It's OK to call this multi-threaded;  the worst thing
// that can happen is that we'll get a bunch of closely
// spaced simulated oveflows, but that's OK, in fact
// probably good as it would exercise the overflow code
// under contention.
bool ParNewGeneration::should_simulate_overflow() {
  if (_overflow_counter-- <= 0) { // just being defensive
    _overflow_counter = ParGCWorkQueueOverflowInterval;
    return true;
  } else {
    return false;
  }
}
#endif

// In case we are using compressed oops, we need to be careful.
// If the object being pushed is an object array, then its length
// field keeps track of the "grey boundary" at which the next
// incremental scan will be done (see ParGCArrayScanChunk).
// When using compressed oops, this length field is kept in the
// lower 32 bits of the erstwhile klass word and cannot be used
// for the overflow chaining pointer (OCP below). As such the OCP
// would itself need to be compressed into the top 32-bits in this
// case. Unfortunately, see below, in the event that we have a
// promotion failure, the node to be pushed on the list can be
// outside of the Java heap, so the heap-based pointer compression
// would not work (we would have potential aliasing between C-heap
// and Java-heap pointers). For this reason, when using compressed
// oops, we simply use a worker-thread-local, non-shared overflow
// list in the form of a growable array, with a slightly different
// overflow stack draining strategy. If/when we start using fat
// stacks here, we can go back to using (fat) pointer chains
// (although some performance comparisons would be useful since
// single global lists have their own performance disadvantages
// as we were made painfully aware not long ago, see 6786503).
#define BUSY (oop(0x1aff1aff))


void ParNewGeneration::push_on_overflow_list(oop from_space_obj, ParScanThreadState* par_scan_state) {
  assert(is_in_reserved(from_space_obj), "Should be from this generation");

  if (ParGCUseLocalOverflow) {
    // In the case of compressed oops, we use a private, not-shared
    // overflow stack.
    par_scan_state->push_on_overflow_stack(from_space_obj);
  } else {
    assert(!UseCompressedOops, "Error");
    // if the object has been forwarded to itself, then we cannot
    // use the klass pointer for the linked list.  Instead we have
    // to allocate an oopDesc in the C-Heap and use that for the linked list.
    // XXX This is horribly inefficient when a promotion failure occurs
    // and should be fixed. XXX FIX ME !!!
#ifndef PRODUCT
    Atomic::inc_ptr(&_num_par_pushes);
    assert(_num_par_pushes > 0, "Tautology");
#endif

    if (from_space_obj->forwardee() == from_space_obj) {
      oopDesc* listhead = NEW_C_HEAP_ARRAY(oopDesc, 1);
      listhead->forward_to(from_space_obj);
      from_space_obj = listhead;
    }

    oop observed_overflow_list = _overflow_list;
    oop cur_overflow_list;
    do {
      cur_overflow_list = observed_overflow_list;
      if (cur_overflow_list != BUSY) {
        from_space_obj->set_klass_to_list_ptr(cur_overflow_list);
      } else {
        from_space_obj->set_klass_to_list_ptr(NULL);
      }
      observed_overflow_list = (oop)Atomic::cmpxchg_ptr(from_space_obj, &_overflow_list, cur_overflow_list);
    } while (cur_overflow_list != observed_overflow_list);
  }

}

/**
 * 从临时溢出栈中取出若干对象转存到指定Gc线程的工作队列中,以便Gc线程扫描复制它们的引用对象(属性)
 */
bool ParNewGeneration::take_from_overflow_list(ParScanThreadState* par_scan_state) {
  bool res;

  if (ParGCUseLocalOverflow) {
    res = par_scan_state->take_from_overflow_stack();
  } else {
    assert(!UseCompressedOops, "Error");
    res = take_from_overflow_list_work(par_scan_state);
  }
  return res;
}


// *NOTE*: The overflow list manipulation code here and
// in CMSCollector:: are very similar in shape,
// except that in the CMS case we thread the objects
// directly into the list via their mark word, and do
// not need to deal with special cases below related
// to chunking of object arrays and promotion failure
// handling.
// CR 6797058 has been filed to attempt consolidation of
// the common code.
// Because of the common code, if you make any changes in
// the code below, please check the CMS version to see if
// similar changes might be needed.
// See CMSCollector::par_take_from_overflow_list() for
// more extensive documentation comments.
bool ParNewGeneration::take_from_overflow_list_work(ParScanThreadState* par_scan_state) {
  ObjToScanQueue* work_q = par_scan_state->work_queue();

  //计算从临时溢出栈中转存出对象的数量
  size_t objsFromOverflow = MIN2((size_t)(work_q->max_elems() - work_q->size())/4, (size_t)ParGCDesiredObjsFromOverflowList);

  assert(!UseCompressedOops, "Error");
  assert(par_scan_state->overflow_stack() == NULL, "Error");

  if (_overflow_list == NULL) return false;

  // Otherwise, there was something there; try claiming the list.
  oop prefix = (oop)Atomic::xchg_ptr(BUSY, &_overflow_list);
  // Trim off a prefix of at most objsFromOverflow items
  Thread* tid = Thread::current();
  size_t spin_count = (size_t)ParallelGCThreads;
  size_t sleep_time_millis = MAX2((size_t)1, objsFromOverflow/100);

  //临时溢出栈中没有可处理的对象(扫描复制其引用对象),则等待-重试若干次，否则取出所有可处理的对象
  for (size_t spin = 0; prefix == BUSY && spin < spin_count; spin++) {
    // someone grabbed it before we did ...
    // ... we spin for a short while...
    os::sleep(tid, sleep_time_millis, false);
    if (_overflow_list == NULL) {
      // nothing left to take
      return false;
    } else if (_overflow_list != BUSY) {
     // try and grab the prefix
     prefix = (oop)Atomic::xchg_ptr(BUSY, &_overflow_list);
    }
  }

  //经过等待-重试若干次之后,临时溢出栈中仍然没有可处理的对象(扫描复制其引用对象),则返回
  if (prefix == NULL || prefix == BUSY) {
     // Nothing to take or waited long enough
     if (prefix == NULL) {
       // Write back the NULL in case we overwrote it with BUSY above
       // and it is still the same value.
       (void) Atomic::cmpxchg_ptr(NULL, &_overflow_list, BUSY);
     }
     return false;
  }

  //确保临时溢出栈中有可处理的对象(扫描复制其引用对象)
  assert(prefix != NULL && prefix != BUSY, "Error");

  //从临时溢出栈中搜索一定数量的可处理对象(扫描复制其引用对象)
  size_t i = 1;
  oop cur = prefix;
  while (i < objsFromOverflow && cur->klass_or_null() != NULL) {
    i++; cur = oop(cur->klass());
  }

  if (cur->klass_or_null() == NULL) {	//从临时溢出栈中取出的可处理对象都可转存到当前Gc线程的工作队列中
    // Write back the NULL in lieu of the BUSY we wrote
    // above and it is still the same value.
    if (_overflow_list == BUSY) {
      (void) Atomic::cmpxchg_ptr(NULL, &_overflow_list, BUSY);
    }
  } else {	//从临时溢出栈中取出的可处理对象不能都转存到当前Gc线程的工作队列中,则将剩余的对象重新放回临时溢出栈
    assert(cur->klass_or_null() != BUSY, "Error");
    oop suffix = oop(cur->klass());       // suffix will be put back on global list
    cur->set_klass_to_list_ptr(NULL);     // break off suffix
    // It's possible that the list is still in the empty(busy) state
    // we left it in a short while ago; in that case we may be
    // able to place back the suffix.
    oop observed_overflow_list = _overflow_list;
    oop cur_overflow_list = observed_overflow_list;
    bool attached = false;
    while (observed_overflow_list == BUSY || observed_overflow_list == NULL) {
      observed_overflow_list = (oop) Atomic::cmpxchg_ptr(suffix, &_overflow_list, cur_overflow_list);
      if (cur_overflow_list == observed_overflow_list) {
        attached = true;
        break;
      } else cur_overflow_list = observed_overflow_list;
    }

    if (!attached) {
      // Too bad, someone else got in in between; we'll need to do a splice.
      // Find the last item of suffix list
      oop last = suffix;
      while (last->klass_or_null() != NULL) {
        last = oop(last->klass());
      }
      // Atomically prepend suffix to current overflow list
      observed_overflow_list = _overflow_list;
      do {
        cur_overflow_list = observed_overflow_list;
        if (cur_overflow_list != BUSY) {
          // Do the splice ...
          last->set_klass_to_list_ptr(cur_overflow_list);
        } else { // cur_overflow_list == BUSY
          last->set_klass_to_list_ptr(NULL);
        }
        observed_overflow_list =
          (oop)Atomic::cmpxchg_ptr(suffix, &_overflow_list, cur_overflow_list);
      } while (cur_overflow_list != observed_overflow_list);
    }
  }

  //将从临时溢出栈中取出的可处理对象转存到当前Gc线程的工作队列中
  assert(prefix != NULL && prefix != BUSY, "program logic");
  cur = prefix;
  ssize_t n = 0;
  while (cur != NULL) {
    oop obj_to_push = cur->forwardee();
    oop next        = oop(cur->klass_or_null());
    cur->set_klass(obj_to_push->klass());
    // This may be an array object that is self-forwarded. In that case, the list pointer
    // space, cur, is not in the Java heap, but rather in the C-heap and should be freed.
    if (!is_in_reserved(cur)) {
      // This can become a scaling bottleneck when there is work queue overflow coincident
      // with promotion failure.
      oopDesc* f = cur;
      FREE_C_HEAP_ARRAY(oopDesc, f);
    } else if (par_scan_state->should_be_partially_scanned(obj_to_push, cur)) {
      assert(arrayOop(cur)->length() == 0, "entire array remaining to be scanned");
      obj_to_push = cur;
    }
    bool ok = work_q->push(obj_to_push);
    assert(ok, "Should have succeeded");
    cur = next;
    n++;
  }

  TASKQUEUE_STATS_ONLY(par_scan_state->note_overflow_refill(n));
#ifndef PRODUCT
  assert(_num_par_pushes >= n, "Too many pops?");
  Atomic::add_ptr(-(intptr_t)n, &_num_par_pushes);
#endif
  return true;
}
#undef BUSY

/**
 * 创建软/弱引用处理器
 */
void ParNewGeneration::ref_processor_init()
{
  if (_ref_processor == NULL) {
    // Allocate and initialize a reference processor
    _ref_processor =
      new ReferenceProcessor(_reserved,                  // span
                             ParallelRefProcEnabled && (ParallelGCThreads > 1), // mt processing
                             (int) ParallelGCThreads,    // mt processing degree
                             refs_discovery_is_mt(),     // mt discovery
                             (int) ParallelGCThreads,    // mt discovery degree
                             refs_discovery_is_atomic(), // atomic_discovery
                             NULL,                       // is_alive_non_header
                             false);                     // write barrier for next field updates
  }
}

/**
 * 当前内存管理器的名称
 */
const char* ParNewGeneration::name() const {
  return "par new generation";
}

/**
 * 当前内存管理器是否已用于当前JVM的年轻代
 */
bool ParNewGeneration::in_use() {
  return UseParNewGC && ParallelGCThreads > 0;
}
