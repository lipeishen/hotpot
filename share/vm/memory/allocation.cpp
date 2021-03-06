/*
 * Copyright (c) 1997, 2013, Oracle and/or its affiliates. All rights reserved.
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
#include "memory/allocation.hpp"
#include "memory/allocation.inline.hpp"
#include "memory/resourceArea.hpp"
#include "runtime/atomic.hpp"
#include "runtime/os.hpp"
#include "runtime/task.hpp"
#include "runtime/threadCritical.hpp"
#include "services/memTracker.hpp"
#include "utilities/ostream.hpp"

#ifdef TARGET_OS_FAMILY_linux
# include "os_linux.inline.hpp"
#endif
#ifdef TARGET_OS_FAMILY_solaris
# include "os_solaris.inline.hpp"
#endif
#ifdef TARGET_OS_FAMILY_windows
# include "os_windows.inline.hpp"
#endif
#ifdef TARGET_OS_FAMILY_bsd
# include "os_bsd.inline.hpp"
#endif

void* StackObj::operator new(size_t size)  { ShouldNotCallThis(); return 0; };
void  StackObj::operator delete(void* p)   { ShouldNotCallThis(); };
void* _ValueObj::operator new(size_t size)  { ShouldNotCallThis(); return 0; };
void  _ValueObj::operator delete(void* p)   { ShouldNotCallThis(); };

void* ResourceObj::operator new(size_t size, allocation_type type, MEMFLAGS flags) {
  address res;
  switch (type) {
   case C_HEAP:
    res = (address)AllocateHeap(size, flags, CALLER_PC);
    DEBUG_ONLY(set_allocation_type(res, C_HEAP);)
    break;
   case RESOURCE_AREA:
    // new(size) sets allocation type RESOURCE_AREA.
    res = (address)operator new(size);
    break;
   default:
    ShouldNotReachHere();
  }
  return res;
}

void ResourceObj::operator delete(void* p) {
  assert(((ResourceObj *)p)->allocated_on_C_heap(),
         "delete only allowed for C_HEAP objects");
  DEBUG_ONLY(((ResourceObj *)p)->_allocation_t[0] = (uintptr_t)badHeapOopVal;)
  FreeHeap(p);
}

#ifdef ASSERT
void ResourceObj::set_allocation_type(address res, allocation_type type) {
    // Set allocation type in the resource object
    uintptr_t allocation = (uintptr_t)res;
    assert((allocation & allocation_mask) == 0, "address should be aligned to 4 bytes at least");
    assert(type <= allocation_mask, "incorrect allocation type");
    ResourceObj* resobj = (ResourceObj *)res;
    resobj->_allocation_t[0] = ~(allocation + type);
    if (type != STACK_OR_EMBEDDED) {
      // Called from operator new() and CollectionSetChooser(),
      // set verification value.
      resobj->_allocation_t[1] = (uintptr_t)&(resobj->_allocation_t[1]) + type;
    }
}

ResourceObj::allocation_type ResourceObj::get_allocation_type() const {
    assert(~(_allocation_t[0] | allocation_mask) == (uintptr_t)this, "lost resource object");
    return (allocation_type)((~_allocation_t[0]) & allocation_mask);
}

bool ResourceObj::is_type_set() const {
    allocation_type type = (allocation_type)(_allocation_t[1] & allocation_mask);
    return get_allocation_type()  == type &&
           (_allocation_t[1] - type) == (uintptr_t)(&_allocation_t[1]);
}

ResourceObj::ResourceObj() { // default constructor
    if (~(_allocation_t[0] | allocation_mask) != (uintptr_t)this) {
      // Operator new() is not called for allocations
      // on stack and for embedded objects.
      set_allocation_type((address)this, STACK_OR_EMBEDDED);
    } else if (allocated_on_stack()) { // STACK_OR_EMBEDDED
      // For some reason we got a value which resembles
      // an embedded or stack object (operator new() does not
      // set such type). Keep it since it is valid value
      // (even if it was garbage).
      // Ignore garbage in other fields.
    } else if (is_type_set()) {
      // Operator new() was called and type was set.
      assert(!allocated_on_stack(),
             err_msg("not embedded or stack, this(" PTR_FORMAT ") type %d a[0]=(" PTR_FORMAT ") a[1]=(" PTR_FORMAT ")",
                     this, get_allocation_type(), _allocation_t[0], _allocation_t[1]));
    } else {
      // Operator new() was not called.
      // Assume that it is embedded or stack object.
      set_allocation_type((address)this, STACK_OR_EMBEDDED);
    }
    _allocation_t[1] = 0; // Zap verification value
}

ResourceObj::ResourceObj(const ResourceObj& r) { // default copy constructor
    // Used in ClassFileParser::parse_constant_pool_entries() for ClassFileStream.
    // Note: garbage may resembles valid value.
    assert(~(_allocation_t[0] | allocation_mask) != (uintptr_t)this || !is_type_set(),
           err_msg("embedded or stack only, this(" PTR_FORMAT ") type %d a[0]=(" PTR_FORMAT ") a[1]=(" PTR_FORMAT ")",
                   this, get_allocation_type(), _allocation_t[0], _allocation_t[1]));
    set_allocation_type((address)this, STACK_OR_EMBEDDED);
    _allocation_t[1] = 0; // Zap verification value
}

ResourceObj& ResourceObj::operator=(const ResourceObj& r) { // default copy assignment
    // Used in InlineTree::ok_to_inline() for WarmCallInfo.
    assert(allocated_on_stack(),
           err_msg("copy only into local, this(" PTR_FORMAT ") type %d a[0]=(" PTR_FORMAT ") a[1]=(" PTR_FORMAT ")",
                   this, get_allocation_type(), _allocation_t[0], _allocation_t[1]));
    // Keep current _allocation_t value;
    return *this;
}

ResourceObj::~ResourceObj() {
    // allocated_on_C_heap() also checks that encoded (in _allocation) address == this.
    if (!allocated_on_C_heap()) { // ResourceObj::delete() will zap _allocation for C_heap.
      _allocation_t[0] = (uintptr_t)badHeapOopVal; // zap type
    }
}
#endif // ASSERT


void trace_heap_malloc(size_t size, const char* name, void* p) {
  // A lock is not needed here - tty uses a lock internally
  tty->print_cr("Heap malloc " INTPTR_FORMAT " " SIZE_FORMAT " %s", p, size, name == NULL ? "" : name);
}


void trace_heap_free(void* p) {
  // A lock is not needed here - tty uses a lock internally
  tty->print_cr("Heap free   " INTPTR_FORMAT, p);
}

bool warn_new_operator = false; // see vm_main

//--------------------------------------------------------------------------------------
// ChunkPool implementation  chunk:程序分块

// MT-safe pool of chunks to reduce malloc/free thrashing(抖动)
// NB: not using Mutex because pools are used before Threads are initialized
// 这里没有使用信号量是因为池子在使用时候，线程还没有创建
class ChunkPool: public CHeapObj<mtInternal> {
  Chunk*       _first;        // first cached Chunk; its first word points to next chunk 链表的头指针
  size_t       _num_chunks;   // number of unused chunks in pool    未分配的数量
  size_t       _num_used;     // number of chunks currently checked out 已分配的数量
  const size_t _size;         // size of each chunk (must be uniform) 单个chunk的固定大小

  // Our three static pools   三种尺寸的静态资源池 大，中，小
  static ChunkPool* _large_pool;
  static ChunkPool* _medium_pool;
  static ChunkPool* _small_pool;

  // return first element or null
  void* get_first() {
    Chunk* c = _first;
    if (_first) {
      _first = _first->next();
      _num_chunks--;
    }
    return c;
  }

 public:
  // All chunks in a ChunkPool has the same size  同一个资源池中的chunk初始化的大小一样
   ChunkPool(size_t size) : _size(size) { _first = NULL; _num_chunks = _num_used = 0; }

  // Allocate a new chunk from the pool (might expand the pool)  从资源池中分配出来一个chunk,有可能需要扩展资源池
  _NOINLINE_ void* allocate(size_t bytes, AllocFailType alloc_failmode) {
    assert(bytes == _size, "bad size");
    void* p = NULL;
    // No VM lock can be taken inside ThreadCritical lock, so os::malloc
    // should be done outside ThreadCritical lock due to NMT  ThreadCritical 继承自StackObj
    { ThreadCritical tc;
      _num_used++;
      p = get_first();
    }
    if (p == NULL) p = os::malloc(bytes, mtChunk, CURRENT_PC);
    if (p == NULL && alloc_failmode == AllocFailStrategy::EXIT_OOM) {
      vm_exit_out_of_memory(bytes, "ChunkPool::allocate");
    }
    return p;
  }

  // Return a chunk to the pool  chunkPool回收chunk,对于程序系统来说是释放资源，而资源池来说是获得资源
  void free(Chunk* chunk) {
    assert(chunk->length() + Chunk::aligned_overhead_size() == _size, "bad size");
    ThreadCritical tc;
    _num_used--;

    // Add chunk to list  //向chunk链头部插入回收的chunk
    chunk->set_next(_first);
    _first = chunk;
    _num_chunks++;
  }

  // Prune the pool
  void free_all_but(size_t n) {
    Chunk* cur = NULL;
    Chunk* next;
    {
    // if we have more than n chunks, free all of them
    ThreadCritical tc;
    if (_num_chunks > n) {
      // free chunks at end of queue, for better locality
        cur = _first;
      for (size_t i = 0; i < (n - 1) && cur != NULL; i++) cur = cur->next();

      if (cur != NULL) {
          next = cur->next();
        cur->set_next(NULL);
        cur = next;

          _num_chunks = n;
        }
      }
    }

    // Free all remaining chunks, outside of ThreadCritical
    // to avoid deadlock with NMT
        while(cur != NULL) {
          next = cur->next();
      os::free(cur, mtChunk);
          cur = next;
        }
      }

  // Accessors to preallocated pool's  通过方法获取三种资源池，如果没有初始化，提示 ‘must be initialized’
  static ChunkPool* large_pool()  { assert(_large_pool  != NULL, "must be initialized"); return _large_pool;  }
  static ChunkPool* medium_pool() { assert(_medium_pool != NULL, "must be initialized"); return _medium_pool; }
  static ChunkPool* small_pool()  { assert(_small_pool  != NULL, "must be initialized"); return _small_pool;  }
//Chunk::aligned_overhead_size() : 返回chunk的占用的size
  static void initialize() {
    //   init_size  =  1*K  - slack, // Size of first chunk
    //medium_size= 10*K  - slack, // Size of medium-sized chunk
    //size       = 32*K  - slack, // Default size of an Arena chunk (following the first)
    //non_pool_size = init_size + 32
    // slack      = 20,
    // chunk 的大小有以上几种，是枚举类型
    _large_pool  = new ChunkPool(Chunk::size        + Chunk::aligned_overhead_size());
    _medium_pool = new ChunkPool(Chunk::medium_size + Chunk::aligned_overhead_size());
    _small_pool  = new ChunkPool(Chunk::init_size   + Chunk::aligned_overhead_size());
  }

  static void clean() {
    enum { BlocksToKeep = 5 };
     _small_pool->free_all_but(BlocksToKeep);
     _medium_pool->free_all_but(BlocksToKeep);
     _large_pool->free_all_but(BlocksToKeep);
  }
};

//类静态成员变量的初始化
ChunkPool* ChunkPool::_large_pool  = NULL;
ChunkPool* ChunkPool::_medium_pool = NULL;
ChunkPool* ChunkPool::_small_pool  = NULL;

void chunkpool_init() {
  ChunkPool::initialize();
}

void
Chunk::clean_chunk_pool() {
  ChunkPool::clean();
}


//--------------------------------------------------------------------------------------
// ChunkPoolCleaner implementation
// chunk资源池清理者，定期(5000ms)去清理chunkpool

class ChunkPoolCleaner : public PeriodicTask {
  enum { CleaningInterval = 5000 };      // cleaning interval in ms

 public:
   ChunkPoolCleaner() : PeriodicTask(CleaningInterval) {}
   void task() {
     ChunkPool::clean();
   }
};

//--------------------------------------------------------------------------------------
// Chunk implementation
// 定义类Chunk 的new() 操作实现
void* Chunk::operator new(size_t requested_size, AllocFailType alloc_failmode, size_t length) {
  // requested_size is equal to sizeof(Chunk) but in order for the arena
  // allocations to come out aligned as expected the size must be aligned
  // to expected arean alignment.
  // expect requested_size but if sizeof(Chunk) doesn't match isn't proper size we must align it.
  assert(ARENA_ALIGN(requested_size) == aligned_overhead_size(), "Bad alignment");
  size_t bytes = ARENA_ALIGN(requested_size) + length;
  switch (length) {
    // length 是资源池定义的三种尺寸的大小，按照已有的方法去分配空间
   case Chunk::size:        return ChunkPool::large_pool()->allocate(bytes, alloc_failmode);
   case Chunk::medium_size: return ChunkPool::medium_pool()->allocate(bytes, alloc_failmode);
   case Chunk::init_size:   return ChunkPool::small_pool()->allocate(bytes, alloc_failmode);
   // 请求的length 不是资源池定义的尺寸，由操作系统类OS调用malloc方法进行分配
   default: {
     void *p =  os::malloc(bytes, mtChunk, CALLER_PC);//bytes:大小，mtChunk：内存标记，CALLER_PC：地址
     if (p == NULL && alloc_failmode == AllocFailStrategy::EXIT_OOM) {//分配失败的处理
       vm_exit_out_of_memory(bytes, "Chunk::new");
     }
     return p;
   }
  }
}
// 定义类Chunk 的delete() 操作实现
void Chunk::operator delete(void* p) {
  Chunk* c = (Chunk*)p;
  switch (c->length()) {
   case Chunk::size:        ChunkPool::large_pool()->free(c); break;
   case Chunk::medium_size: ChunkPool::medium_pool()->free(c); break;
   case Chunk::init_size:   ChunkPool::small_pool()->free(c); break;
   default:                 os::free(c, mtChunk);
  }
}
//Chunk 构造函数
Chunk::Chunk(size_t length) : _len(length) { //const类型的参数放在参数列表中初始化
  _next = NULL;         // Chain on the linked list
}

//chop 是切除的意思，顾名思义，就是从chunk资源链表中切除该节点（链表的删除操作，释放删除节点占用的空间）
void Chunk::chop() {
  Chunk *k = this;
  while( k ) {
    Chunk *tmp = k->next();
    // clear out this chunk (to detect allocation bugs)
    if (ZapResourceArea) memset(k->bottom(), badResourceValue, k->length());
    delete k;                   // Free chunk (was malloc'd)
    k = tmp;
  }
}
//切除当前节点的下一个节点
void Chunk::next_chop() {
  _next->chop();
  _next = NULL;
}


void Chunk::start_chunk_pool_cleaner_task() {
#ifdef ASSERT
  static bool task_created = false;
  assert(!task_created, "should not start chuck pool cleaner twice");
  task_created = true;
#endif
  ChunkPoolCleaner* cleaner = new ChunkPoolCleaner();
  cleaner->enroll();//激活定时清理任务
}

//------------------------------Arena------------------------------------------
NOT_PRODUCT(volatile jint Arena::_instance_count = 0;)  //记录当前Arena实例的数量
//有参数构造函数
Arena::Arena(size_t init_size) {
  size_t round_size = (sizeof (char *)) - 1;
  init_size = (init_size+round_size) & ~round_size;
  _first = _chunk = new (AllocFailStrategy::EXIT_OOM, init_size) Chunk(init_size);
  _hwm = _chunk->bottom();      // Save the cached hwm, max
  _max = _chunk->top();
  set_size_in_bytes(init_size);//设置Arena占用的size
  NOT_PRODUCT(Atomic::inc(&_instance_count);)
}
//无参数构造函数
Arena::Arena() {
  _first = _chunk = new (AllocFailStrategy::EXIT_OOM, Chunk::init_size) Chunk(Chunk::init_size);
  _hwm = _chunk->bottom();      // Save the cached hwm, max
  _max = _chunk->top();
  set_size_in_bytes(Chunk::init_size);
  NOT_PRODUCT(Atomic::inc(&_instance_count);)
}
//对象拷贝
Arena *Arena::move_contents(Arena *copy) {
  copy->destruct_contents();//清理copyz占用的空间
  copy->_chunk = _chunk;
  copy->_hwm   = _hwm;
  copy->_max   = _max;
  copy->_first = _first;

  // workaround rare racing condition, which could double count
  // the arena size by native memory tracking
  size_t size = size_in_bytes(); //获取当前Arena对象占用的空间
  set_size_in_bytes(0); //先把当前Arena对象占用的空间置为0
  copy->set_size_in_bytes(size); //最后当前Arena对象占用的空间设为copy对象占用的空间大小
  // Destroy original arena
  reset();
  return copy;            // Return Arena with contents
}
//析构函数
Arena::~Arena() {
  destruct_contents();
  NOT_PRODUCT(Atomic::dec(&_instance_count);)
}

void* Arena::operator new(size_t size) {
  assert(false, "Use dynamic memory type binding");
  return NULL;
}

void* Arena::operator new (size_t size, const std::nothrow_t&  nothrow_constant) {
  assert(false, "Use dynamic memory type binding");
  return NULL;
}

  // dynamic memory type binding
void* Arena::operator new(size_t size, MEMFLAGS flags) {
#ifdef ASSERT
  void* p = (void*)AllocateHeap(size, flags|otArena, CALLER_PC);
  if (PrintMallocFree) trace_heap_malloc(size, "Arena-new", p);
  return p;
#else
  return (void *) AllocateHeap(size, flags|otArena, CALLER_PC);
#endif
}

void* Arena::operator new(size_t size, const std::nothrow_t& nothrow_constant, MEMFLAGS flags) {
#ifdef ASSERT
  void* p = os::malloc(size, flags|otArena, CALLER_PC);
  if (PrintMallocFree) trace_heap_malloc(size, "Arena-new", p);
  return p;
#else
  return os::malloc(size, flags|otArena, CALLER_PC);
#endif
}

void Arena::operator delete(void* p) {
  FreeHeap(p);
}

// Destroy this arenas contents and reset to empty
//清空Arena占用的空间
void Arena::destruct_contents() {
  if (UseMallocOnly && _first != NULL) {
    char* end = _first->next() ? _first->top() : _hwm;
    free_malloced_objects(_first, _first->bottom(), end, _hwm);
  }
  // reset size before chop to avoid a rare racing condition
  // that can have total arena memory exceed total chunk memory
  set_size_in_bytes(0);
  _first->chop();
  reset();
}

// This is high traffic method, but many calls actually don't
// change the size 此方法是频繁调用的方法，主要作用是重新设置占用空间的大小
void Arena::set_size_in_bytes(size_t size) {
  if (_size_in_bytes != size) {
    _size_in_bytes = size;
    MemTracker::record_arena_size((address)this, size); //跟踪当前Arena的内存
   }
}

// Total of all Chunks in  use of arena 计算arena中所有Chunk已经使用的空间
size_t Arena::used() const {
  size_t sum = _chunk->length() - (_max-_hwm); // Size leftover in this Chunk 剩余的大小
  register Chunk *k = _first;
  while( k != _chunk) {         // Whilst（同时） have Chunks in a row
    sum += k->length();         // Total size of this Chunk
    k = k->next();              // Bump（碰撞，撞击） along to next Chunk
  }
  return sum;                   // Return total consumed space.
}
//内存溢出提示
void Arena::signal_out_of_memory(size_t sz, const char* whence) const {
  vm_exit_out_of_memory(sz, whence);
}

// Grow a new Chunk 为当前Arena对象中chunk资源链中增加一个Chunk
//注意返回类型的void指针
void* Arena::grow(size_t x, AllocFailType alloc_failmode) {
  // Get minimal required size.  Either real big, or even bigger for giant objs
  size_t len = MAX2(x, (size_t) Chunk::size);//返回较大的那个值

  Chunk *k = _chunk;            // Get filled-up chunk address
  _chunk = new (alloc_failmode, len) Chunk(len);

  if (_chunk == NULL) {
    return NULL;
  }
  if (k) k->set_next(_chunk);   // Append new chunk to end of linked list
  else _first = _chunk;
  _hwm  = _chunk->bottom();     // Save the cached hwm, max
  _max =  _chunk->top();
  set_size_in_bytes(size_in_bytes() + len);//在已有的占用空间上增加新加chunk的大小len
  void* result = _hwm;
  _hwm += x;
  return result;
}



// Reallocate storage in Arena.
void *Arena::Arealloc(void* old_ptr, size_t old_size, size_t new_size, AllocFailType alloc_failmode) {
  assert(new_size >= 0, "bad size");
  if (new_size == 0) return NULL;
#ifdef ASSERT
  if (UseMallocOnly) {
    // always allocate a new object  (otherwise we'll free this one twice)
    char* copy = (char*)Amalloc(new_size, alloc_failmode);
    if (copy == NULL) {
      return NULL;
    }
    size_t n = MIN2(old_size, new_size);
    if (n > 0) memcpy(copy, old_ptr, n);
    Afree(old_ptr,old_size);    // Mostly done to keep stats accurate
    return copy;
  }
#endif
  char *c_old = (char*)old_ptr; // Handy name
  // Stupid fast special case
  if( new_size <= old_size ) {  // Shrink in-place
    if( c_old+old_size == _hwm) // Attempt to free the excess bytes
      _hwm = c_old+new_size;    // Adjust hwm
    return c_old;
  }

  // make sure that new_size is legal
  size_t corrected_new_size = ARENA_ALIGN(new_size);

  // See if we can resize in-place
  if( (c_old+old_size == _hwm) &&       // Adjusting recent thing
      (c_old+corrected_new_size <= _max) ) {      // Still fits where it sits
    _hwm = c_old+corrected_new_size;      // Adjust hwm
    return c_old;               // Return old pointer
  }

  // Oops, got to relocate guts
  void *new_ptr = Amalloc(new_size, alloc_failmode);
  if (new_ptr == NULL) {
    return NULL;
  }
  memcpy( new_ptr, c_old, old_size );
  Afree(c_old,old_size);        // Mostly done to keep stats accurate
  return new_ptr;
}


// Determine if pointer belongs to this Arena or not.
//原理是从链表中查找元素
bool Arena::contains( const void *ptr ) const {
#ifdef ASSERT
  if (UseMallocOnly) {
    // really slow, but not easy to make fast
    if (_chunk == NULL) return false;
    char** bottom = (char**)_chunk->bottom();
    for (char** p = (char**)_hwm - 1; p >= bottom; p--) {
      if (*p == ptr) return true;
    }
    for (Chunk *c = _first; c != NULL; c = c->next()) {
      if (c == _chunk) continue;  // current chunk has been processed
      char** bottom = (char**)c->bottom();
      for (char** p = (char**)c->top() - 1; p >= bottom; p--) {
        if (*p == ptr) return true;
      }
    }
    return false;
  }
#endif
  if( (void*)_chunk->bottom() <= ptr && ptr < (void*)_hwm )
    return true;                // Check for in this chunk
  for (Chunk *c = _first; c; c = c->next()) {
    if (c == _chunk) continue;  // current chunk has been processed
    if ((void*)c->bottom() <= ptr && ptr < (void*)c->top()) {
      return true;              // Check for every chunk in Arena
    }
  }
  return false;                 // Not in any Chunk, so not in Arena
}


#ifdef ASSERT
void* Arena::malloc(size_t size) {
  assert(UseMallocOnly, "shouldn't call");
  // use malloc, but save pointer in res. area for later freeing
  char** save = (char**)internal_malloc_4(sizeof(char*));
  return (*save = (char*)os::malloc(size, mtChunk));
}

// for debugging with UseMallocOnly
void* Arena::internal_malloc_4(size_t x) {
  assert( (x&(sizeof(char*)-1)) == 0, "misaligned size" );
  check_for_overflow(x, "Arena::internal_malloc_4");
  if (_hwm + x > _max) {
    return grow(x);
  } else {
    char *old = _hwm;
    _hwm += x;
    return old;
  }
}
#endif


//--------------------------------------------------------------------------------------
// Non-product code

#ifndef PRODUCT
// The global operator new should never be called since it will usually indicate
// a memory leak.  Use CHeapObj as the base class of such objects to make it explicit
// that they're allocated on the C heap.
// Commented out in product version to avoid conflicts with third-party C++ native code.
// %% note this is causing a problem on solaris debug build. the global
// new is being called from jdk source and causing data corruption.
// src/share/native/sun/awt/font/fontmanager/textcache/hsMemory.cpp::hsSoftNew
// define CATCH_OPERATOR_NEW_USAGE if you want to use this.
#ifdef CATCH_OPERATOR_NEW_USAGE
void* operator new(size_t size){
  static bool warned = false;
  if (!warned && warn_new_operator)
    warning("should not call global (default) operator new");
  warned = true;
  return (void *) AllocateHeap(size, "global operator new");
}
#endif

void AllocatedObj::print() const       { print_on(tty); }
void AllocatedObj::print_value() const { print_value_on(tty); }

void AllocatedObj::print_on(outputStream* st) const {
  st->print_cr("AllocatedObj(" INTPTR_FORMAT ")", this);
}

void AllocatedObj::print_value_on(outputStream* st) const {
  st->print("AllocatedObj(" INTPTR_FORMAT ")", this);
}

julong Arena::_bytes_allocated = 0;

void Arena::inc_bytes_allocated(size_t x) { inc_stat_counter(&_bytes_allocated, x); }

AllocStats::AllocStats() {
  start_mallocs      = os::num_mallocs;
  start_frees        = os::num_frees;
  start_malloc_bytes = os::alloc_bytes;
  start_mfree_bytes  = os::free_bytes;
  start_res_bytes    = Arena::_bytes_allocated;
}

julong  AllocStats::num_mallocs() { return os::num_mallocs - start_mallocs; }
julong  AllocStats::alloc_bytes() { return os::alloc_bytes - start_malloc_bytes; }
julong  AllocStats::num_frees()   { return os::num_frees - start_frees; }
julong  AllocStats::free_bytes()  { return os::free_bytes - start_mfree_bytes; }
julong  AllocStats::resource_bytes() { return Arena::_bytes_allocated - start_res_bytes; }
void    AllocStats::print() {
  tty->print_cr(UINT64_FORMAT " mallocs (" UINT64_FORMAT "MB), "
                UINT64_FORMAT" frees (" UINT64_FORMAT "MB), " UINT64_FORMAT "MB resrc",
                num_mallocs(), alloc_bytes()/M, num_frees(), free_bytes()/M, resource_bytes()/M);
}


// debugging code
inline void Arena::free_all(char** start, char** end) {
  for (char** p = start; p < end; p++) if (*p) os::free(*p);
}
//清楚Arena 中chunk 所占用的空间
void Arena::free_malloced_objects(Chunk* chunk, char* hwm, char* max, char* hwm2) {
  assert(UseMallocOnly, "should not call");
  // free all objects malloced since resource mark was created; resource area
  // contains their addresses
  if (chunk->next()) {
    // this chunk is full, and some others too
    for (Chunk* c = chunk->next(); c != NULL; c = c->next()) {
      char* top = c->top();
      if (c->next() == NULL) {
        top = hwm2;     // last junk is only used up to hwm2
        assert(c->contains(hwm2), "bad hwm2");
      }
      free_all((char**)c->bottom(), (char**)top);
    }
    assert(chunk->contains(hwm), "bad hwm");
    assert(chunk->contains(max), "bad max");
    free_all((char**)hwm, (char**)max);
  } else {
    // this chunk was partially used
    assert(chunk->contains(hwm), "bad hwm");
    assert(chunk->contains(hwm2), "bad hwm2");
    free_all((char**)hwm, (char**)hwm2);
  }
}


ReallocMark::ReallocMark() {
#ifdef ASSERT
  Thread *thread = ThreadLocalStorage::get_thread_slow();
  _nesting = thread->resource_area()->nesting();
#endif
}

void ReallocMark::check() {
#ifdef ASSERT
  if (_nesting != Thread::current()->resource_area()->nesting()) {
    fatal("allocation bug: array could grow within nested ResourceMark");
  }
#endif
}

#endif // Non-product
