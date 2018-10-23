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
#include "gc_implementation/shared/collectorCounters.hpp"
#include "gc_implementation/shared/gcPolicyCounters.hpp"
#include "gc_implementation/shared/spaceDecorator.hpp"
#include "memory/defNewGeneration.inline.hpp"
#include "memory/gcLocker.inline.hpp"
#include "memory/genCollectedHeap.hpp"
#include "memory/genOopClosures.inline.hpp"
#include "memory/generationSpec.hpp"
#include "memory/iterator.hpp"
#include "memory/referencePolicy.hpp"
#include "memory/space.inline.hpp"
#include "oops/instanceRefKlass.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/java.hpp"
#include "utilities/copy.hpp"
#include "utilities/stack.inline.hpp"
#ifdef TARGET_OS_FAMILY_linux
# include "thread_linux.inline.hpp"
#endif
#ifdef TARGET_OS_FAMILY_solaris
# include "thread_solaris.inline.hpp"
#endif
#ifdef TARGET_OS_FAMILY_windows
# include "thread_windows.inline.hpp"
#endif
#ifdef TARGET_OS_FAMILY_bsd
# include "thread_bsd.inline.hpp"
#endif

//
// DefNewGeneration functions.

// Methods of protected closure types.

DefNewGeneration::IsAliveClosure::IsAliveClosure(Generation* g) : _g(g) {
  assert(g->level() == 0, "Optimized for youngest gen.");
}
void DefNewGeneration::IsAliveClosure::do_object(oop p) {
  assert(false, "Do not call.");
}
bool DefNewGeneration::IsAliveClosure::do_object_b(oop p) {
  return (HeapWord*)p >= _g->reserved().end() || p->is_forwarded();
}

DefNewGeneration::KeepAliveClosure::
KeepAliveClosure(ScanWeakRefClosure* cl) : _cl(cl) {
  GenRemSet* rs = GenCollectedHeap::heap()->rem_set();
  assert(rs->rs_kind() == GenRemSet::CardTable, "Wrong rem set kind.");
  _rs = (CardTableRS*)rs;
}

void DefNewGeneration::KeepAliveClosure::do_oop(oop* p)       { DefNewGeneration::KeepAliveClosure::do_oop_work(p); }
void DefNewGeneration::KeepAliveClosure::do_oop(narrowOop* p) { DefNewGeneration::KeepAliveClosure::do_oop_work(p); }


DefNewGeneration::FastKeepAliveClosure::
FastKeepAliveClosure(DefNewGeneration* g, ScanWeakRefClosure* cl) :
  DefNewGeneration::KeepAliveClosure(cl) {
  _boundary = g->reserved().end();
}

void DefNewGeneration::FastKeepAliveClosure::do_oop(oop* p)       { DefNewGeneration::FastKeepAliveClosure::do_oop_work(p); }
void DefNewGeneration::FastKeepAliveClosure::do_oop(narrowOop* p) { DefNewGeneration::FastKeepAliveClosure::do_oop_work(p); }

DefNewGeneration::EvacuateFollowersClosure::
EvacuateFollowersClosure(GenCollectedHeap* gch, int level,
                         ScanClosure* cur, ScanClosure* older) :
  _gch(gch), _level(level),
  _scan_cur_or_nonheap(cur), _scan_older(older)
{}

void DefNewGeneration::EvacuateFollowersClosure::do_void() {
  do {
    _gch->oop_since_save_marks_iterate(_level, _scan_cur_or_nonheap,
                                       _scan_older);
  } while (!_gch->no_allocs_since_save_marks(_level));
}

DefNewGeneration::FastEvacuateFollowersClosure::
FastEvacuateFollowersClosure(GenCollectedHeap* gch, int level,
                             DefNewGeneration* gen,
                             FastScanClosure* cur, FastScanClosure* older) :
  _gch(gch), _level(level), _gen(gen),
  _scan_cur_or_nonheap(cur), _scan_older(older)
{}

/**
 * 通过迭代分析对象的引用关系来进行垃圾回收
 */
void DefNewGeneration::FastEvacuateFollowersClosure::do_void() {
  do {
	  //分析各内存代上的存活的对象的引用关系(垃圾回收)
    _gch->oop_since_save_marks_iterate(_level, _scan_cur_or_nonheap, _scan_older);

  } while (!_gch->no_allocs_since_save_marks(_level));

  guarantee(_gen->promo_failure_scan_is_complete(), "Failed to finish scan");
}

ScanClosure::ScanClosure(DefNewGeneration* g, bool gc_barrier) :
  OopsInGenClosure(g), _g(g), _gc_barrier(gc_barrier)
{
  assert(_g->level() == 0, "Optimized for youngest generation");
  _boundary = _g->reserved().end();
}

void ScanClosure::do_oop(oop* p)       { ScanClosure::do_oop_work(p); }
void ScanClosure::do_oop(narrowOop* p) { ScanClosure::do_oop_work(p); }

FastScanClosure::FastScanClosure(DefNewGeneration* g, bool gc_barrier) :
  OopsInGenClosure(g), _g(g), _gc_barrier(gc_barrier)
{
  assert(_g->level() == 0, "Optimized for youngest generation");
  _boundary = _g->reserved().end();
}

void FastScanClosure::do_oop(oop* p)       { FastScanClosure::do_oop_work(p); }
void FastScanClosure::do_oop(narrowOop* p) { FastScanClosure::do_oop_work(p); }

ScanWeakRefClosure::ScanWeakRefClosure(DefNewGeneration* g) :
  OopClosure(g->ref_processor()), _g(g)
{
  assert(_g->level() == 0, "Optimized for youngest generation");
  _boundary = _g->reserved().end();
}

void ScanWeakRefClosure::do_oop(oop* p)       { ScanWeakRefClosure::do_oop_work(p); }
void ScanWeakRefClosure::do_oop(narrowOop* p) { ScanWeakRefClosure::do_oop_work(p); }

void FilteringClosure::do_oop(oop* p)       { FilteringClosure::do_oop_work(p); }
void FilteringClosure::do_oop(narrowOop* p) { FilteringClosure::do_oop_work(p); }

DefNewGeneration::DefNewGeneration(ReservedSpace rs,
                                   size_t initial_size,
                                   int level,
                                   const char* policy)
  : Generation(rs, initial_size, level),
    _promo_failure_drain_in_progress(false),
    _should_allocate_from_space(false)
{
  MemRegion cmr((HeapWord*)_virtual_space.low(), (HeapWord*)_virtual_space.high());

  Universe::heap()->barrier_set()->resize_covered_region(cmr);

  if (GenCollectedHeap::heap()->collector_policy()->has_soft_ended_eden()) {
    _eden_space = new ConcEdenSpace(this);
  } else {
    _eden_space = new EdenSpace(this);
  }

  _from_space = new ContiguousSpace();
  _to_space   = new ContiguousSpace();

  if (_eden_space == NULL || _from_space == NULL || _to_space == NULL)
    vm_exit_during_initialization("Could not allocate a new gen space");

  // Compute the maximum eden and survivor space sizes. These sizes
  // are computed assuming the entire reserved space is committed.
  // These values are exported as performance counters.
  uintx alignment = GenCollectedHeap::heap()->collector_policy()->min_alignment();
  uintx size = _virtual_space.reserved_size();

  //计算From区和To区的最大大小
  _max_survivor_size = compute_survivor_size(size, alignment);
  //Eden区最大大小
  _max_eden_size = size - (2*_max_survivor_size);

  // allocate the performance counters

  // Generation counters -- generation 0, 3 subspaces
  _gen_counters = new GenerationCounters("new", 0, 3, &_virtual_space);
  _gc_counters = new CollectorCounters(policy, 0);

  _eden_counters = new CSpaceCounters("eden", 0, _max_eden_size, _eden_space, _gen_counters);
  _from_counters = new CSpaceCounters("s0", 1, _max_survivor_size, _from_space, _gen_counters);
  _to_counters = new CSpaceCounters("s1", 2, _max_survivor_size, _to_space, _gen_counters);

  compute_space_boundaries(0, SpaceDecorator::Clear, SpaceDecorator::Mangle);

  update_counters();

  _next_gen = NULL;
  _tenuring_threshold = MaxTenuringThreshold;
  _pretenure_size_threshold_words = PretenureSizeThreshold >> LogHeapWordSize;

  printf("%s[%d] [tid: %lu]: 默认年青代内存代管理器[%d:DefNewGeneration]{max_survivor_size=%lu, max_eden_size=%lu, pretenure_size_threshold_words(一次可申请的最大内存块)=%lu}...\n", __FILE__, __LINE__, pthread_self(),
		  level, _max_survivor_size, _max_eden_size, _pretenure_size_threshold_words);

}

/**
 * 确定三个内存区Eden/From/To的物理地址边界
 */
void DefNewGeneration::compute_space_boundaries(uintx minimum_eden_size,
                                                bool clear_space,
                                                bool mangle_space) {
  uintx alignment = GenCollectedHeap::heap()->collector_policy()->min_alignment();

  // If the spaces are being cleared (only done at heap initialization
  // currently), the survivor spaces need not be empty.
  // Otherwise, no care is taken for used areas in the survivor spaces
  // so check.
  assert(clear_space || (to()->is_empty() && from()->is_empty()),
    "Initialization of the survivor spaces assumes these are empty");

  // Compute sizes
  uintx size = _virtual_space.committed_size();
  uintx survivor_size = compute_survivor_size(size, alignment);
  uintx eden_size = size - (2*survivor_size);
  assert(eden_size > 0 && survivor_size <= eden_size, "just checking");

  if (eden_size < minimum_eden_size) {
    // May happen due to 64Kb rounding, if so adjust eden size back up
    minimum_eden_size = align_size_up(minimum_eden_size, alignment);
    uintx maximum_survivor_size = (size - minimum_eden_size) / 2;
    uintx unaligned_survivor_size =
      align_size_down(maximum_survivor_size, alignment);
    survivor_size = MAX2(unaligned_survivor_size, alignment);
    eden_size = size - (2*survivor_size);
    assert(eden_size > 0 && survivor_size <= eden_size, "just checking");
    assert(eden_size >= minimum_eden_size, "just checking");
  }

  char *eden_start = _virtual_space.low();
  char *from_start = eden_start + eden_size;
  char *to_start   = from_start + survivor_size;
  char *to_end     = to_start   + survivor_size;

  assert(to_end == _virtual_space.high(), "just checking");
  assert(Space::is_aligned((HeapWord*)eden_start), "checking alignment");
  assert(Space::is_aligned((HeapWord*)from_start), "checking alignment");
  assert(Space::is_aligned((HeapWord*)to_start),   "checking alignment");

  MemRegion edenMR((HeapWord*)eden_start, (HeapWord*)from_start);
  MemRegion fromMR((HeapWord*)from_start, (HeapWord*)to_start);
  MemRegion toMR  ((HeapWord*)to_start, (HeapWord*)to_end);

  // A minimum eden size implies that there is a part of eden that
  // is being used and that affects the initialization of any
  // newly formed eden.
  bool live_in_eden = minimum_eden_size > 0;

  // If not clearing the spaces, do some checking to verify that
  // the space are already mangled.
  if (!clear_space) {
    // Must check mangling before the spaces are reshaped.  Otherwise,
    // the bottom or end of one space may have moved into another
    // a failure of the check may not correctly indicate which space
    // is not properly mangled.
    if (ZapUnusedHeapArea) {
      HeapWord* limit = (HeapWord*) _virtual_space.high();
      eden()->check_mangled_unused_area(limit);
      from()->check_mangled_unused_area(limit);
        to()->check_mangled_unused_area(limit);
    }
  }

  // Reset the spaces for their new regions.
  eden()->initialize(edenMR,
                     clear_space && !live_in_eden,
                     SpaceDecorator::Mangle);
  // If clear_space and live_in_eden, we will not have cleared any
  // portion of eden above its top. This can cause newly
  // expanded space not to be mangled if using ZapUnusedHeapArea.
  // We explicitly do such mangling here.
  if (ZapUnusedHeapArea && clear_space && live_in_eden && mangle_space) {
    eden()->mangle_unused_area();
  }

  from()->initialize(fromMR, clear_space, mangle_space);
  to()->initialize(toMR, clear_space, mangle_space);

  // Set next compaction spaces.
  eden()->set_next_compaction_space(from());
  // The to-space is normally empty before a compaction so need
  // not be considered.  The exception is during promotion
  // failure handling when to-space can contain live objects.
  from()->set_next_compaction_space(NULL);
}

void DefNewGeneration::swap_spaces() {
  ContiguousSpace* s = from();
  _from_space        = to();
  _to_space          = s;
  eden()->set_next_compaction_space(from());
  // The to-space is normally empty before a compaction so need
  // not be considered.  The exception is during promotion
  // failure handling when to-space can contain live objects.
  from()->set_next_compaction_space(NULL);

  if (UsePerfData) {
    CSpaceCounters* c = _from_counters;
    _from_counters = _to_counters;
    _to_counters = c;
  }
}

bool DefNewGeneration::expand(size_t bytes) {
  MutexLocker x(ExpandHeap_lock);
  HeapWord* prev_high = (HeapWord*) _virtual_space.high();
  bool success = _virtual_space.expand_by(bytes);
  if (success && ZapUnusedHeapArea) {
    // Mangle newly committed space immediately because it
    // can be done here more simply that after the new
    // spaces have been computed.
    HeapWord* new_high = (HeapWord*) _virtual_space.high();
    MemRegion mangle_region(prev_high, new_high);
    SpaceMangler::mangle_region(mangle_region);
  }

  // Do not attempt an expand-to-the reserve size.  The
  // request should properly observe the maximum size of
  // the generation so an expand-to-reserve should be
  // unnecessary.  Also a second call to expand-to-reserve
  // value potentially can cause an undue expansion.
  // For example if the first expand fail for unknown reasons,
  // but the second succeeds and expands the heap to its maximum
  // value.
  if (GC_locker::is_active()) {
    if (PrintGC && Verbose) {
      gclog_or_tty->print_cr("Garbage collection disabled, "
        "expanded heap instead");
    }
  }

  return success;
}

/**
 * 一次Gc之后，重新调整(扩展/缩小)年青代的物理空间
 * 根据年老代当前容量/NewRatio/当前非守护线程数量/NewSizeThreadIncrease来确定年青代的容量
 *
 * 	 扩展年青代的物理空间: From/To区完全空闲
 * 	 缩小年青代的物理空间: Eden/From/To区完全空闲
 */
void DefNewGeneration::compute_new_size() {
  // This is called after a gc that includes the following generation
  // (which is required to exist.)  So from-space will normally be empty.
  // Note that we check both spaces, since if scavenge failed they revert roles.
  // If not we bail out (otherwise we would have to relocate the objects)
  if (!from()->is_empty() || !to()->is_empty()) {
    return;
  }

  int next_level = level() + 1;
  GenCollectedHeap* gch = GenCollectedHeap::heap();
  assert(next_level < gch->_n_gens,
         "DefNewGeneration cannot be an oldest gen");
  //老年代的内存容量
  Generation* next_gen = gch->_gens[next_level];
  size_t old_size = next_gen->capacity();

  //年青代的当前容量/最小容量/最大容量
  size_t new_size_before = _virtual_space.committed_size();
  size_t min_new_size = spec()->init_size();
  size_t max_new_size = reserved().byte_size();
  assert(min_new_size <= new_size_before && new_size_before <= max_new_size, "just checking");

  // All space sizes must be multiples of Generation::GenGrain.
  size_t alignment = Generation::GenGrain;

  //根据参数NewRatio、NewSizeThreadIncrease来确定年青代的新容量
  size_t desired_new_size = old_size/NewRatio;
  int threads_count = Threads::number_of_non_daemon_threads();
  size_t thread_increase_size = threads_count * NewSizeThreadIncrease;
  desired_new_size = align_size_up(desired_new_size + thread_increase_size, alignment);

  // Adjust new generation size
  desired_new_size = MAX2(MIN2(desired_new_size, max_new_size), min_new_size);
  assert(desired_new_size <= max_new_size, "just checking");

  bool changed = false;

  //扩张年青代的物理空间
  if (desired_new_size > new_size_before) {
    size_t change = desired_new_size - new_size_before;
    assert(change % alignment == 0, "just checking");
    if (expand(change)) {
       changed = true;
    }
    // If the heap failed to expand to the desired size,
    // "changed" will be false.  If the expansion failed
    // (and at this point it was expected to succeed),
    // ignore the failure (leaving "changed" as false).
  }

  //如果当前Eden区完全空闲，则缩小年青代的物理空间
  if (desired_new_size < new_size_before && eden()->is_empty()) {
    // bail out of shrinking if objects in eden
    size_t change = new_size_before - desired_new_size;
    assert(change % alignment == 0, "just checking");
    _virtual_space.shrink_by(change);
    changed = true;
  }

  if (changed) {
    // The spaces have already been mangled at this point but
    // may not have been cleared (set top = bottom) and should be.
    // Mangling was done when the heap was being expanded.
    compute_space_boundaries(eden()->used(),
                             SpaceDecorator::Clear,
                             SpaceDecorator::DontMangle);
    MemRegion cmr((HeapWord*)_virtual_space.low(),
                  (HeapWord*)_virtual_space.high());
    Universe::heap()->barrier_set()->resize_covered_region(cmr);
    if (Verbose && PrintGC) {
      size_t new_size_after  = _virtual_space.committed_size();
      size_t eden_size_after = eden()->capacity();
      size_t survivor_size_after = from()->capacity();
      gclog_or_tty->print("New generation size " SIZE_FORMAT "K->"
        SIZE_FORMAT "K [eden="
        SIZE_FORMAT "K,survivor=" SIZE_FORMAT "K]",
        new_size_before/K, new_size_after/K,
        eden_size_after/K, survivor_size_after/K);
      if (WizardMode) {
        gclog_or_tty->print("[allowed " SIZE_FORMAT "K extra for %d threads]",
          thread_increase_size/K, threads_count);
      }
      gclog_or_tty->cr();
    }
  }
}

void DefNewGeneration::object_iterate_since_last_GC(ObjectClosure* cl) {
  // $$$ This may be wrong in case of "scavenge failure"?
  eden()->object_iterate(cl);
}

/**
 * 当前内存代管理器只能用于年青代
 */
void DefNewGeneration::younger_refs_iterate(OopsInGenClosure* cl) {
  assert(false, "NYI -- are you sure you want to call this?");
}

/**
 * 年青代的内存容量
 */
size_t DefNewGeneration::capacity() const {
  return eden()->capacity() + from()->capacity();  // to() is only used during scavenge
}

/**
 * 年青代的内存使用量
 */
size_t DefNewGeneration::used() const {
  return eden()->used() + from()->used();      // to() is only used during scavenge
}

/**
 * 年青代的内存空闲量
 */
size_t DefNewGeneration::free() const {
  return eden()->free() + from()->free();      // to() is only used during scavenge
}

size_t DefNewGeneration::max_capacity() const {
  const size_t alignment = GenCollectedHeap::heap()->collector_policy()->min_alignment();
  const size_t reserved_bytes = reserved().byte_size();
  return reserved_bytes - compute_survivor_size(reserved_bytes, alignment);
}

size_t DefNewGeneration::unsafe_max_alloc_nogc() const {
  return eden()->free();
}

size_t DefNewGeneration::capacity_before_gc() const {
  return eden()->capacity();
}

size_t DefNewGeneration::contiguous_available() const {
  return eden()->free();
}


HeapWord** DefNewGeneration::top_addr() const { return eden()->top_addr(); }
HeapWord** DefNewGeneration::end_addr() const { return eden()->end_addr(); }

void DefNewGeneration::object_iterate(ObjectClosure* blk) {
  eden()->object_iterate(blk);
  from()->object_iterate(blk);
}

/**
 * 依次遍历Eden/From/To区上分配的对象
 */
void DefNewGeneration::space_iterate(SpaceClosure* blk,
                                     bool usedOnly) {
  blk->do_space(eden());
  blk->do_space(from());
  blk->do_space(to());
}

/**
 * 内存管理器从From区分配内存(最后一次尝试)
 */
HeapWord* DefNewGeneration::allocate_from_space(size_t size) {
  HeapWord* result = NULL;
  if (Verbose && PrintGCDetails) {
    gclog_or_tty->print("DefNewGeneration::allocate_from_space(%u):"
                        "  will_fail: %s"
                        "  heap_lock: %s"
                        "  free: " SIZE_FORMAT,
                        size,
                        GenCollectedHeap::heap()->incremental_collection_will_fail(false /* don't consult_young */) ?
                          "true" : "false",
                        Heap_lock->is_locked() ? "locked" : "unlocked",
                        from()->free());
  }

  if (should_allocate_from_space() || GC_locker::is_active_and_needs_gc()) {
    if (Heap_lock->owned_by_self() ||
        (SafepointSynchronize::is_at_safepoint() && Thread::current()->is_VM_thread())) {
      // If the Heap_lock is not locked by this thread, this will be called
      // again later with the Heap_lock held.
      result = from()->allocate(size);
    } else if (PrintGC && Verbose) {
      gclog_or_tty->print_cr("  Heap_lock is not owned by self");
    }
  } else if (PrintGC && Verbose) {
    gclog_or_tty->print_cr("  should_allocate_from_space: NOT");
  }

  if (PrintGC && Verbose) {
    gclog_or_tty->print_cr("  returns %s", result == NULL ? "NULL" : "object");
  }

  return result;
}

/**
 * 默认的年青代内存管理器并不支持扩展内存再分配方式
 */
HeapWord* DefNewGeneration::expand_and_allocate(size_t size,
                                                bool   is_tlab,
                                                bool   parallel) {
  // We don't attempt to expand the young generation (but perhaps we should.)
  return allocate(size, is_tlab);
}

/**
 * 执行本内存代的垃圾回收
 */
void DefNewGeneration::collect(bool   full,
                               bool   clear_all_soft_refs,
                               size_t size,
                               bool   is_tlab) {
  assert(full || size > 0, "otherwise we don't want to collect");

  GenCollectedHeap* gch = GenCollectedHeap::heap();

  //下一个内存代
  _next_gen = gch->next_gen(this);
  //当前内存代管理器只用于年青代
  assert(_next_gen != NULL, "This must be the youngest gen, and not the only gen");

  //检查回收当前内存代是否安全,若不安全则放弃回收该内存代,并通知内存堆管理器关闭当前的增量式垃圾回收方式
  if (!collection_attempt_is_safe()) {
    if (Verbose && PrintGCDetails) {
      gclog_or_tty->print(" :: Collection attempt not safe :: ");
    }
    gch->set_incremental_collection_failed(); // Slight lie: we did not even attempt one
    return;
  }

  assert(to()->is_empty(), "Else not collection_attempt_is_safe");

  init_assuming_no_promotion_failure();

  TraceTime t1("GC", PrintGC && !PrintGCDetails, true, gclog_or_tty);

  //记录该Gc之前,内存堆的使用量
  size_t gch_prev_used = gch->used();

  SpecializationStats::clear();

  // These can be shared for all code paths
  IsAliveClosure is_alive(this);
  ScanWeakRefClosure scan_weak_ref(this);

  age_table()->clear();
  to()->clear(SpaceDecorator::Mangle);

  gch->rem_set()->prepare_for_younger_refs_iterate(false);

  assert(gch->no_allocs_since_save_marks(0), "save marks have not been newly set.");

  // Not very pretty.
  CollectorPolicy* cp = gch->collector_policy();

  FastScanClosure fsc_with_no_gc_barrier(this, false);
  FastScanClosure fsc_with_gc_barrier(this, true);

  set_promo_failure_scan_stack_closure(&fsc_with_no_gc_barrier);
  //用于分析对象的引用并进行回收
  FastEvacuateFollowersClosure evacuate_followers(gch, _level, this,
                                                  &fsc_with_no_gc_barrier,
                                                  &fsc_with_gc_barrier);

  assert(gch->no_allocs_since_save_marks(0), "save marks have not been newly set.");

  //遍历当前内存代上的所有根对象
  gch->gen_process_strong_roots(_level,
                                true,  // Process younger gens, if any, as strong roots.
                                true,  // activate StrongRootsScope
                                false, // not collecting perm generation.
                                SharedHeap::SO_AllClasses,
                                &fsc_with_no_gc_barrier,
                                true,   // walk *all* scavengable nmethods
                                &fsc_with_gc_barrier);

  //迭代递归遍历当前内存代上的所有根对象的引用对象，并进行垃圾回收
  evacuate_followers.do_void();

  //清理软引用对象
  FastKeepAliveClosure keep_alive(this, &scan_weak_ref);
  ReferenceProcessor* rp = ref_processor();
  rp->setup_policy(clear_all_soft_refs);
  rp->process_discovered_references(&is_alive, &keep_alive, &evacuate_followers, NULL);

  if (!promotion_failed()) {	//当前内存代(年青代)在本次Gc过程中没有发生对象升级失败

    //Eden/From区清零
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

    //交换From/To区
    swap_spaces();

    assert(to()->is_empty(), "to space should be empty now");

    //重新计算对象可进入下一个内存代的存活时间阈值
    _tenuring_threshold = age_table()->compute_tenuring_threshold(to()->capacity()/HeapWordSize);

    // A successful scavenge should restart the GC time limit count which is
    // for full GC's.
    AdaptiveSizePolicy* size_policy = gch->gen_policy()->size_policy();
    size_policy->reset_gc_overhead_limit_count();
    if (PrintGC && !PrintGCDetails) {
      gch->print_heap_change(gch_prev_used);
    }

    assert(!gch->incremental_collection_failed(), "Should be clear");

  } else {	//当前内存代(年青代)在本次Gc过程中发生了对象升级失败
    assert(_promo_failure_scan_stack.is_empty(), "post condition");

    _promo_failure_scan_stack.clear(true); // Clear cached segments.

    remove_forwarding_pointers();
    if (PrintGCDetails) {
      gclog_or_tty->print(" (promotion failed) ");
    }
    // Add to-space to the list of space to compact
    // when a promotion failure has occurred.  In that
    // case there can be live objects in to-space
    // as a result of a partial evacuation of eden
    // and from-space.
    swap_spaces();   // For uniformity wrt ParNewGeneration.
    //设置From区下一个可压缩内存区为To区,以便在下一次的Full Gc中压缩调整
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
  SpecializationStats::print();

  // We need to use a monotonically non-deccreasing time in ms
  // or we will see time-warp warnings and os::javaTimeMillis()
  // does not guarantee monotonicity.
  jlong now = os::javaTimeNanos() / NANOSECS_PER_MILLISEC;
  update_time_of_last_gc(now);
}

class RemoveForwardPointerClosure: public ObjectClosure {
public:
  void do_object(oop obj) {
    obj->init_mark();
  }
};

void DefNewGeneration::init_assuming_no_promotion_failure() {
  _promotion_failed = false;
  from()->set_next_compaction_space(NULL);
}

void DefNewGeneration::remove_forwarding_pointers() {
  RemoveForwardPointerClosure rspc;
  eden()->object_iterate(&rspc);
  from()->object_iterate(&rspc);

  // Now restore saved marks, if any.
  assert(_objs_with_preserved_marks.size() == _preserved_marks_of_objs.size(), "should be the same");
  while (!_objs_with_preserved_marks.is_empty()) {
    oop obj   = _objs_with_preserved_marks.pop();
    markOop m = _preserved_marks_of_objs.pop();
    obj->set_mark(m);
  }

  _objs_with_preserved_marks.clear(true);
  _preserved_marks_of_objs.clear(true);
}

void DefNewGeneration::preserve_mark(oop obj, markOop m) {
  assert(promotion_failed() && m->must_be_preserved_for_promotion_failure(obj),
         "Oversaving!");
  _objs_with_preserved_marks.push(obj);
  _preserved_marks_of_objs.push(m);
}

void DefNewGeneration::preserve_mark_if_necessary(oop obj, markOop m) {
  if (m->must_be_preserved_for_promotion_failure(obj)) {
    preserve_mark(obj, m);
  }
}

/**
 * 处理指定对象升级到下一个内存代的失败
 */
void DefNewGeneration::handle_promotion_failure(oop old) {
  if (PrintPromotionFailure && !_promotion_failed) {
    gclog_or_tty->print(" (promotion failure size = " SIZE_FORMAT ") ",
                        old->size());
  }

  _promotion_failed = true;

  preserve_mark_if_necessary(old, old->mark());
  // forward to self
  old->forward_to(old);

  _promo_failure_scan_stack.push(old);

  if (!_promo_failure_drain_in_progress) {
    // prevent recursion in copy_to_survivor_space()
    _promo_failure_drain_in_progress = true;
    drain_promo_failure_scan_stack();
    _promo_failure_drain_in_progress = false;
  }
}

/**
 * 将Eden/From区中的某个对象复制到To区或年老代中(当前内存代正在GC)
 */
oop DefNewGeneration::copy_to_survivor_space(oop old) {
  assert(is_in_reserved(old) && !old->is_forwarded(), "shouldn't be scavenging this oop");
  //复制对象大小
  size_t s = old->size();
  oop obj = NULL;

  //如果复制对象的存活时间还未超过设置的阈值,则优先将其拷贝到To区
  if (old->age() < tenuring_threshold()) {
    obj = (oop) to()->allocate(s);
  }


  if (obj == NULL) {	//To区内存不够存储该对象或该对象可以升级到下一个内存代(旧生代)
    obj = _next_gen->promote(old, s);
    if (obj == NULL) {
      handle_promotion_failure(old);
      return old;
    }
  } else {		//将复制对象拷贝到To区
    // Prefetch beyond obj
    const intx interval = PrefetchCopyIntervalInBytes;
    Prefetch::write(obj, interval);

    //复制旧对象的值到新对象中
    Copy::aligned_disjoint_words((HeapWord*)old, (HeapWord*)obj, s);

    //增加对象的存活时间
    obj->incr_age();
    age_table()->add(obj, s);
  }

  // Done, insert forward pointer to obj in this header
  old->forward_to(obj);

  return obj;
}

/**
 * 处理当前内存代垃圾回收过程中升级失败的对象
 */
void DefNewGeneration::drain_promo_failure_scan_stack() {
  while (!_promo_failure_scan_stack.is_empty()) {
     oop obj = _promo_failure_scan_stack.pop();
     //处理该对象的所有引用对象
     obj->oop_iterate(_promo_failure_scan_stack_closure);
  }
}

void DefNewGeneration::save_marks() {
  eden()->set_saved_mark();
  to()->set_saved_mark();
  from()->set_saved_mark();
}


void DefNewGeneration::reset_saved_marks() {
  eden()->reset_saved_mark();
  to()->reset_saved_mark();
  from()->reset_saved_mark();
}


bool DefNewGeneration::no_allocs_since_save_marks() {
  assert(eden()->saved_mark_at_top(), "Violated spec - alloc in eden");
  assert(from()->saved_mark_at_top(), "Violated spec - alloc in from");
  return to()->saved_mark_at_top();
}

#define DefNew_SINCE_SAVE_MARKS_DEFN(OopClosureType, nv_suffix) \
                                                                \
void DefNewGeneration::                                         \
oop_since_save_marks_iterate##nv_suffix(OopClosureType* cl) {   \
  cl->set_generation(this);                                     \
  eden()->oop_since_save_marks_iterate##nv_suffix(cl);          \
  to()->oop_since_save_marks_iterate##nv_suffix(cl);            \
  from()->oop_since_save_marks_iterate##nv_suffix(cl);          \
  cl->reset_generation();                                       \
  save_marks();                                                 \
}

ALL_SINCE_SAVE_MARKS_CLOSURES(DefNew_SINCE_SAVE_MARKS_DEFN)

#undef DefNew_SINCE_SAVE_MARKS_DEFN

void DefNewGeneration::contribute_scratch(ScratchBlock*& list, Generation* requestor,
                                         size_t max_alloc_words) {
  if (requestor == this || _promotion_failed) return;
  assert(requestor->level() > level(), "DefNewGeneration must be youngest");

  /* $$$ Assert this?  "trace" is a "MarkSweep" function so that's not appropriate.
  if (to_space->top() > to_space->bottom()) {
    trace("to_space not empty when contribute_scratch called");
  }
  */

  ContiguousSpace* to_space = to();
  assert(to_space->end() >= to_space->top(), "pointers out of order");
  size_t free_words = pointer_delta(to_space->end(), to_space->top());
  if (free_words >= MinFreeScratchWords) {
    ScratchBlock* sb = (ScratchBlock*)to_space->top();
    sb->num_words = free_words;
    sb->next = list;
    list = sb;
  }
}

void DefNewGeneration::reset_scratch() {
  // If contributing scratch in to_space, mangle all of
  // to_space if ZapUnusedHeapArea.  This is needed because
  // top is not maintained while using to-space as scratch.
  if (ZapUnusedHeapArea) {
    to()->mangle_unused_area_complete();
  }
}

/**
 * 检查本内存代当前进行垃圾回收是否安全
 * 	1).To区空闲
 * 	2).下一个内存代的可用空间能够容纳当前内存代的所有对象(用于对象升级)
 */
bool DefNewGeneration::collection_attempt_is_safe() {
  if (!to()->is_empty()) {
    if (Verbose && PrintGCDetails) {
      gclog_or_tty->print(" :: to is not empty :: ");
    }
    return false;
  }
  if (_next_gen == NULL) {
    GenCollectedHeap* gch = GenCollectedHeap::heap();
    _next_gen = gch->next_gen(this);
    assert(_next_gen != NULL,
           "This must be the youngest gen, and not the only gen");
  }
  return _next_gen->promotion_attempt_is_safe(used());
}

/**
 * Minor/Full GC操作的后置处理
 * 	1.Full Gc:
 * 	   1).Eden区不完全空闲 && 下一次无法执行Minor Gc
 * 	      a).禁止增量式Gc(Minor Gc)
 * 	      b).开启From区分配
 * 	   2).Eden区完全空闲 || 下一次可安全执行Minor Gc
 * 	      a).开启增量式Gc(Minor Gc)
 * 	      b).禁止From区分配
 *
 * 	2.Minor GC:
 */
void DefNewGeneration::gc_epilogue(bool full) {
  DEBUG_ONLY(static bool seen_incremental_collection_failed = false;)

  assert(!GC_locker::is_active(), "We should not be executing here");
  // Check if the heap is approaching full after a collection has
  // been done.  Generally the young generation is empty at
  // a minimum at the end of a collection.  If it is not, then
  // the heap is approaching full.
  GenCollectedHeap* gch = GenCollectedHeap::heap();
  if (full) {
    DEBUG_ONLY(seen_incremental_collection_failed = false;)

    if (!collection_attempt_is_safe() && !_eden_space->is_empty()) {
      if (Verbose && PrintGCDetails) {
        gclog_or_tty->print("DefNewEpilogue: cause(%s), full, not safe, set_failed, set_alloc_from, clear_seen",
                            GCCause::to_string(gch->gc_cause()));
      }
      gch->set_incremental_collection_failed(); // Slight lie: a full gc left us in that state
      set_should_allocate_from_space(); // we seem to be running out of space
    } else {
      if (Verbose && PrintGCDetails) {
        gclog_or_tty->print("DefNewEpilogue: cause(%s), full, safe, clear_failed, clear_alloc_from, clear_seen",
                            GCCause::to_string(gch->gc_cause()));
      }
      gch->clear_incremental_collection_failed(); // We just did a full collection
      clear_should_allocate_from_space(); // if set
    }
  } else {
#ifdef ASSERT
    // It is possible that incremental_collection_failed() == true
    // here, because an attempted scavenge did not succeed. The policy
    // is normally expected to cause a full collection which should
    // clear that condition, so we should not be here twice in a row
    // with incremental_collection_failed() == true without having done
    // a full collection in between.
    if (!seen_incremental_collection_failed &&
        gch->incremental_collection_failed()) {
      if (Verbose && PrintGCDetails) {
        gclog_or_tty->print("DefNewEpilogue: cause(%s), not full, not_seen_failed, failed, set_seen_failed",
                            GCCause::to_string(gch->gc_cause()));
      }
      seen_incremental_collection_failed = true;
    } else if (seen_incremental_collection_failed) {
      if (Verbose && PrintGCDetails) {
        gclog_or_tty->print("DefNewEpilogue: cause(%s), not full, seen_failed, will_clear_seen_failed",
                            GCCause::to_string(gch->gc_cause()));
      }
      assert(gch->gc_cause() == GCCause::_scavenge_alot ||
             (gch->gc_cause() == GCCause::_java_lang_system_gc && UseConcMarkSweepGC && ExplicitGCInvokesConcurrent) ||
             !gch->incremental_collection_failed(),
             "Twice in a row");
      seen_incremental_collection_failed = false;
    }
#endif // ASSERT
  }

  if (ZapUnusedHeapArea) {
    eden()->check_mangled_unused_area_complete();
    from()->check_mangled_unused_area_complete();
    to()->check_mangled_unused_area_complete();
  }

  if (!CleanChunkPoolAsync) {
    Chunk::clean_chunk_pool();
  }

  // update the generation and space performance counters
  update_counters();
  gch->collector_policy()->counters()->update_counters();
}

/**
 * 保存当前Eden/From/To区下次分配内存的开始位置
 */
void DefNewGeneration::record_spaces_top() {
  assert(ZapUnusedHeapArea, "Not mangling unused space");
  eden()->set_top_for_allocations();
  to()->set_top_for_allocations();
  from()->set_top_for_allocations();
}


void DefNewGeneration::update_counters() {
  if (UsePerfData) {
    _eden_counters->update_all();
    _from_counters->update_all();
    _to_counters->update_all();
    _gen_counters->update_all();
  }
}

void DefNewGeneration::verify(bool allow_dirty) {
  eden()->verify(allow_dirty);
  from()->verify(allow_dirty);
    to()->verify(allow_dirty);
}

void DefNewGeneration::print_on(outputStream* st) const {
  Generation::print_on(st);
  st->print("  eden");
  eden()->print_on(st);
  st->print("  from");
  from()->print_on(st);
  st->print("  to  ");
  to()->print_on(st);
}


const char* DefNewGeneration::name() const {
  return "def new generation";
}

/**
 * 获取该内存代中第一个可压缩的内存区: Eden区
 */
CompactibleSpace* DefNewGeneration::first_compaction_space() const {
  return eden();
}

/**
 * 从当前的内存代中分配指定大小的内存空间
 * 		1).以并行的方式快速从Eden区分配内存
 * 		2).扩张Eden区的方式分配内存
 * 		3).从From区分配内存
 */
HeapWord* DefNewGeneration::allocate(size_t word_size,
                                     bool is_tlab) {
  // This is the slow-path allocation for the DefNewGeneration.
  // Most allocations are fast-path in compiled code.
  // We try to allocate from the eden.  If that works, we are happy.
  // Note that since DefNewGeneration supports lock-free allocation, we
  // have to use it here, as well.
  HeapWord* result = eden()->par_allocate(word_size);
  if (result != NULL) {
    return result;
  }

  do {
	  //扩张Eden区
    HeapWord* old_limit = eden()->soft_end();
    if (old_limit < eden()->end()) {
      //通知下一个内存代管理器,Eden区使用达到了逻辑(软)限制
      //由下一个内存代管理器来决定Eden区新的软限制位置
      HeapWord* new_limit = next_gen()->allocation_limit_reached(eden(), eden()->top(), word_size);
      if (new_limit != NULL) {
        Atomic::cmpxchg_ptr(new_limit, eden()->soft_end_addr(), old_limit);
      } else {
        assert(eden()->soft_end() == eden()->end(), "invalid state after allocation_limit_reached returned null");
      }
    } else {
      // The allocation failed and the soft limit is equal to the hard limit,
      // there are no reasons to do an attempt to allocate
      assert(old_limit == eden()->end(), "sanity check");
      break;
    }

    //扩张Eden区后,再次尝试快速分配
    result = eden()->par_allocate(word_size);
  } while (result == NULL);

  // If the eden is full and the last collection bailed out, we are running
  // out of heap space, and we try to allocate the from-space, too.
  // allocate_from_space can't be inlined because that would introduce a
  // circular dependency at compile time.
  if (result == NULL) {
    result = allocate_from_space(word_size);
  }

  return result;
}

/**
 * 从Eden区快速分配(支持并发)
 */
HeapWord* DefNewGeneration::par_allocate(size_t word_size, bool is_tlab) {
  return eden()->par_allocate(word_size);
}

void DefNewGeneration::gc_prologue(bool full) {
  // Ensure that _end and _soft_end are the same in eden space.
  eden()->set_soft_end(eden()->end());
}

/**
 * 内存代可分配给线程本地分配缓冲区的空间
 */
size_t DefNewGeneration::tlab_capacity() const {
  return eden()->capacity();
}

/**
 * 内存代当前可分配给线程本地分配缓冲区的最大空间
 */
size_t DefNewGeneration::unsafe_max_tlab_alloc() const {
  return unsafe_max_alloc_nogc();
}