/* -*- mode: C; c-basic-offset: 3; -*- */
/*--------------------------------------------------------------------*/
/*--- Startup: create initial process image on NetBSD              ---*/
/*---                                             initimg-netbsd.c ---*/
/*--------------------------------------------------------------------*/

/*
   This file is part of Valgrind, a dynamic binary instrumentation
   framework.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
   02111-1307, USA.

   The GNU General Public License is contained in the file COPYING.
*/

#if defined(VGO_netbsd)

#include "pub_core_aspacemgr.h"
#include "pub_core_basics.h"
#include "pub_core_vki.h"
#include "pub_core_clientstate.h"
#include "pub_core_debuglog.h"
#include "pub_core_libcassert.h"
#include "pub_core_libcbase.h"
#include "pub_core_libcfile.h"
#include "pub_core_libcprint.h"
#include "pub_core_libcproc.h"
#include "pub_core_mallocfree.h"
#include "pub_core_machine.h"
#include "pub_core_options.h"
#include "pub_core_syscall.h"
#include "pub_core_threadstate.h"
#include "pub_core_tooliface.h"
#include "pub_core_ume.h"
#include "priv_initimg_pathscan.h"
#include "pub_core_initimg.h"         /* self */

/* --- !!! --- EXTERNAL HEADERS start --- !!! --- */
/* This is for ELF types etc, and also the AT_ constants. */
#include <elf.h>
/* --- !!! --- EXTERNAL HEADERS end --- !!! --- */

/*====================================================================*/
/*=== Loading the client                                           ===*/
/*====================================================================*/
static void load_client ( /*MOD*/ExeInfo* info,
                          /*OUT*/HChar *out_exe_name, SizeT out_exe_name_size )
{
   const HChar* exe_name;
   Int    ret;
   SysRes res;

   vg_assert( VG_(args_the_exename) != NULL);
   exe_name = ML_(find_executable)( VG_(args_the_exename) );

   if (!exe_name) {
      VG_(printf)("valgrind: %s: command not found\n", VG_(args_the_exename));
      VG_(exit)(127);      // 127 is Posix NOTFOUND
   }

   ret = VG_(do_exec)(exe_name, info);
   if (ret < 0) {
      VG_(printf)("valgrind: could not execute '%s'\n", exe_name);
      VG_(exit)(1);
   }

   // The client was successfully loaded!  Continue.

   /* Save resolved exename. */
   if (VG_(strlen)(exe_name) + 1 > out_exe_name_size) {
      /* This should not really happen. */
      VG_(printf)("valgrind: execname %s is too long\n", exe_name);
      VG_(exit)(1);
      /*NOTREACHED*/
   }
   VG_(strcpy)(out_exe_name, exe_name);

   /* Get hold of a file descriptor which refers to the client
      executable.  This is needed for attaching to GDB. */
   res = VG_(open)(exe_name, VKI_O_RDONLY, VKI_S_IRUSR);
   if (!sr_isError(res))
      VG_(cl_exec_fd) = sr_Res(res);

   /* Set initial brk values. */
   VG_(brk_base) = VG_(brk_limit) = info->brkbase;
}

/*====================================================================*/
/*=== Setting up the client's environment                          ===*/
/*====================================================================*/

/* Prepare the client's environment.  This is basically a copy of our
 * environment, except:
 *
 *   LD_PRELOAD=$VALGRIND_LIB/vgpreload_core-PLATFORM.so:
 *              ($VALGRIND_LIB/vgpreload_TOOL-PLATFORM.so:)?
 *              $LD_PRELOAD
 *
 * If this is missing, then it is added.
 *
 * Also, remove any binding for VALGRIND_LAUNCHER=.  The client should
 * not be able to see this.
 *
 * If this needs to handle any more variables it should be hacked
 * into something table driven.  The copy is VG_(malloc)'d space.
 */
static HChar** setup_client_env ( HChar** origenv, const HChar* toolname )
{
   vg_assert(origenv);
   vg_assert(toolname);

   const HChar* preload_core    = "vgpreload_core";
   const HChar* ld_preload      = "LD_PRELOAD=";
   const HChar* v_launcher      = VALGRIND_LAUNCHER "=";
   Int    ld_preload_len  = VG_(strlen)( ld_preload );
   Int    v_launcher_len  = VG_(strlen)( v_launcher );
   Bool   ld_preload_done = False;
   Int    vglib_len       = VG_(strlen)(VG_(libdir));
   Bool   debug           = False;

   HChar** cpp;
   HChar** ret;
   HChar*  preload_tool_path;
   Int     envc, i;

   /* Alloc space for the vgpreload_core.so path and vgpreload_<tool>.so
      paths.  We might not need the space for vgpreload_<tool>.so, but it
      doesn't hurt to over-allocate briefly.  The 16s are just cautious
      slop. */
   Int preload_core_path_len = vglib_len + sizeof(preload_core)
                                         + sizeof(VG_PLATFORM) + 16;
   Int preload_tool_path_len = vglib_len + VG_(strlen)(toolname)
                                         + sizeof(VG_PLATFORM) + 16;
   Int preload_string_len    = preload_core_path_len + preload_tool_path_len;
   HChar* preload_string     = VG_(malloc)("initimg-netbsd.sce.1",
                                           preload_string_len);
   /* Determine if there's a vgpreload_<tool>_<platform>.so file, and setup
      preload_string. */
   preload_tool_path = VG_(malloc)("initimg-netbsd.sce.2", preload_tool_path_len);
   VG_(snprintf)(preload_tool_path, preload_tool_path_len,
                 "%s/vgpreload_%s-%s.so", VG_(libdir), toolname, VG_PLATFORM);
   if (VG_(access)(preload_tool_path, True/*r*/, False/*w*/, False/*x*/) == 0) {
      VG_(snprintf)(preload_string, preload_string_len, "%s/%s-%s.so:%s",
                    VG_(libdir), preload_core, VG_PLATFORM, preload_tool_path);
   } else {
      VG_(snprintf)(preload_string, preload_string_len, "%s/%s-%s.so",
                    VG_(libdir), preload_core, VG_PLATFORM);
   }
   VG_(free)(preload_tool_path);

   VG_(debugLog)(2, "initimg", "preload_string:\n");
   VG_(debugLog)(2, "initimg", "  \"%s\"\n", preload_string);

   /* Count the original size of the env */
   if (debug) VG_(printf)("\n\n");
   envc = 0;
   for (cpp = origenv; cpp && *cpp; cpp++) {
      envc++;
      if (debug) VG_(printf)("XXXXXXXXX: BEFORE %s\n", *cpp);
   }

   /* Allocate a new space */
   ret = VG_(malloc) ("initimg-netbsd.sce.3",
                      sizeof(HChar *) * (envc+1+1)); /* 1 new entry + NULL */

   /* copy it over */
   for (cpp = ret; *origenv; ) {
      if (debug) VG_(printf)("XXXXXXXXX: COPY   %s\n", *origenv);
      *cpp++ = *origenv++;
   }
   *cpp = NULL;

   vg_assert(envc == (cpp - ret));

   /* Walk over the new environment, mashing as we go */
   for (cpp = ret; cpp && *cpp; cpp++) {
      if (VG_(memcmp)(*cpp, ld_preload, ld_preload_len) == 0) {
         Int len = VG_(strlen)(*cpp) + preload_string_len;
         HChar *cp = VG_(malloc)("initimg-netbsd.sce.4", len);

         VG_(snprintf)(cp, len, "%s%s:%s",
                       ld_preload, preload_string, (*cpp)+ld_preload_len);

         *cpp = cp;

         ld_preload_done = True;
      }
      if (debug) VG_(printf)("XXXXXXXXX: MASH   %s\n", *cpp);
   }

   /* Add the missing bits */
   if (!ld_preload_done) {
      Int len = ld_preload_len + preload_string_len;
      HChar *cp = VG_(malloc) ("initimg-netbsd.sce.5", len);

      VG_(snprintf)(cp, len, "%s%s", ld_preload, preload_string);

      ret[envc++] = cp;
      if (debug) VG_(printf)("XXXXXXXXX: ADD    %s\n", cp);
   }

   /* ret[0 .. envc-1] is live now. */
   /* Find and remove a binding for VALGRIND_LAUNCHER. */
   for (i = 0; i < envc; i++)
      if (0 == VG_(memcmp)(ret[i], v_launcher, v_launcher_len))
         break;

   if (i < envc) {
      for (; i < envc-1; i++)
         ret[i] = ret[i+1];
      envc--;
   }

   VG_(free)(preload_string);
   ret[envc] = NULL;

   for (i = 0; i < envc; i++) {
      if (debug) VG_(printf)("XXXXXXXXX: FINAL  %s\n", ret[i]);
   }

   return ret;
}

/*====================================================================*/
/*=== Setting up the client's stack                                ===*/
/*====================================================================*/

/* Add a string onto the string table, and return its address */
static HChar *copy_str(HChar **tab, const HChar *str)
{
   HChar *cp = *tab;
   HChar *orig = cp;

   while(*str)
      *cp++ = *str++;
   *cp++ = '\0';

   if (0)
      VG_(printf)("copied %p \"%s\" len %lld\n", orig, orig, (Long)(cp-orig));

   *tab = cp;

   return orig;
}

/* This sets up the client's initial stack, containing the args,
 * environment and aux vector.
 *
 * The format of the stack is:
 *
 * higher address +-----------------+ <- clstack_end
 *                | struct          |
 *                | ps_strings      |
 *                +-----------------+
 *                |                 |
 *                : string table    :
 *                |                 |
 *                +-----------------+
 *                | AT_NULL         |
 *                -                 -
 *                | auxv            |
 *                +-----------------+
 *                | NULL            |
 *                -                 -
 *                | envp            |
 *                +-----------------+
 *                | NULL            |
 *                -                 -
 *                | argv            |
 *                +-----------------+
 *                | argc            |
 * lower address  +-----------------+ <- sp
 *                | undefined       |
 *                :                 :
 *
 * Allocate and create the initial client stack.  It is allocated down from
 * clstack_end, which was previously determined by the address space manager.
 * The returned value is the SP value for the client.
 *
 * The client's auxv is created by copying and modifying our own one.
 */

struct auxv
{
   Word a_type;
   union {
      void *a_ptr;
      Word a_val;
   } u;
};

static
struct auxv *find_auxv(UWord* sp)
{
   sp++;                // skip argc (Nb: is word-sized, not int-sized!)

   while (*sp != 0)     // skip argv
      sp++;
   sp++;

   while (*sp != 0)     // skip env
      sp++;
   sp++;

   return (struct auxv *)sp;
}

static
Addr setup_client_stack( void*  init_sp,
                         HChar** orig_envp,
                         const ExeInfo* info,
                         UInt** client_auxv,
                         struct vki_ps_strings** client_pss,
                         Addr   clstack_end,
                         SizeT  clstack_max_size,
                         const HChar *resolved_exe_name )
{
   vg_assert(VG_IS_PAGE_ALIGNED(clstack_end+1));
   vg_assert(VG_(args_for_client));

   /* use our own auxv as a prototype */
   const struct auxv *orig_auxv = find_auxv(init_sp);

   /* ==================== compute sizes ==================== */

   /* first of all, work out how big the client stack will be */
   unsigned stringsize = 0;

   /* paste on the extra args if the loader needs them (ie, the #!
    * interpreter and its argument) */
   Int argc = 0;
   if (info->interp_name != NULL) {
      argc++;
      stringsize += VG_(strlen)(info->interp_name) + 1;
   }
   if (info->interp_args != NULL) {
      argc++;
      stringsize += VG_(strlen)(info->interp_args) + 1;
   }

   /* now scan the args we're given... */
   stringsize += VG_(strlen)(resolved_exe_name) + 1;

   for (Int i = 0; i < VG_(sizeXA)( VG_(args_for_client) ); i++) {
      argc++;
      stringsize += VG_(strlen)( * (HChar**)
                                   VG_(indexXA)( VG_(args_for_client), i ))
                    + 1;
   }

   /* ...and the environment */
   Int envc = 0;
   for (HChar **cpp = orig_envp; cpp && *cpp; cpp++) {
      envc++;
      stringsize += VG_(strlen)(*cpp) + 1;
   }

   /* now, how big is the auxv? Ref. elf_copyargs() in
    * sys/kern/exec_elf.c:
    *
    * AT_PHDR
    * AT_PHENT
    * AT_PHNUM
    * AT_PAGESZ
    * AT_BASE
    * AT_FLAGS
    * AT_ENTRY
    * AT_EUID
    * AT_RUID
    * AT_EGID
    * AT_RGID
    * AT_STACKBASE
    * AT_SUN_EXECNAME
    * AT_NULL
    */
   unsigned auxsize = sizeof(struct auxv);     /* there's always at least one entry: AT_NULL */
   for (const struct auxv *cauxv = orig_auxv; cauxv->a_type != AT_NULL; cauxv++) {
      if (cauxv->a_type == AT_SUN_EXECNAME)
         stringsize += VG_(strlen)(VG_(args_the_exename)) + 1;
      auxsize += sizeof(*cauxv);
   }

   /* OK, now we know how big the client stack is */
   unsigned stacksize =
      sizeof(Word) +                          /* argc */
      sizeof(HChar **) +                      /* argc[0] == exename */
      sizeof(HChar **)*argc +                 /* argv */
      sizeof(HChar **) +                      /* terminal NULL */
      sizeof(HChar **)*envc +                 /* envp */
      sizeof(HChar **) +                      /* terminal NULL */
      auxsize +                               /* auxv */
      sizeof(struct vki_ps_strings) +         /* pss */
      VG_ROUNDUP(stringsize, sizeof(Word));   /* strings (aligned) */

   if (0) VG_(printf)("stacksize = %u\n", stacksize);

   /* client_SP is the client's stack pointer */
   Addr client_SP = clstack_end - stacksize;
   client_SP = VG_ROUNDDN(client_SP, 16); /* make stack 16 byte aligned */

   /* base of the string table (aligned) */
   HChar *strtab;
   HChar *stringbase;
   stringbase = strtab = (HChar *)clstack_end
                         - sizeof(struct vki_ps_strings)
                         - VG_ROUNDUP(stringsize, sizeof(int));

   Addr clstack_start = VG_PGROUNDDN(client_SP);

   /* The max stack size */
   clstack_max_size = VG_PGROUNDUP(clstack_max_size);

   if (0)
      VG_(printf)("stringsize=%u auxsize=%u stacksize=%u maxsize=0x%lx\n"
                  "clstack_start %p\n"
                  "clstack_end   %p\n",
                  stringsize, auxsize, stacksize, clstack_max_size,
                  (void*)clstack_start, (void*)clstack_end);

   /* ==================== allocate space ==================== */

   { SizeT anon_size   = clstack_end - clstack_start + 1;
     SizeT resvn_size  = clstack_max_size - anon_size;
     Addr  anon_start  = clstack_start;
     Addr  resvn_start = anon_start - resvn_size;
     SizeT inner_HACK  = 0;

     /* So far we've only accounted for space requirements down to the
      * stack pointer.  If this target's ABI requires a redzone below
      * the stack pointer, we need to allocate an extra page, to
      * handle the worst case in which the stack pointer is almost at
      * the bottom of a page, and so there is insufficient room left
      * over to put the redzone in.  In this case the simple thing to
      * do is allocate an extra page, by shrinking the reservation by
      * one page and growing the anonymous area by a corresponding
      * page. */
     vg_assert(VG_STACK_REDZONE_SZB >= 0);
     vg_assert(VG_STACK_REDZONE_SZB < VKI_PAGE_SIZE);
     if (VG_STACK_REDZONE_SZB > 0) {
        vg_assert(resvn_size > VKI_PAGE_SIZE);
        resvn_size -= VKI_PAGE_SIZE;
        anon_start -= VKI_PAGE_SIZE;
        anon_size  += VKI_PAGE_SIZE;
     }

     vg_assert(VG_IS_PAGE_ALIGNED(anon_size));
     vg_assert(VG_IS_PAGE_ALIGNED(resvn_size));
     vg_assert(VG_IS_PAGE_ALIGNED(anon_start));
     vg_assert(VG_IS_PAGE_ALIGNED(resvn_start));
     vg_assert(resvn_start == clstack_end + 1 - clstack_max_size);

#    ifdef ENABLE_INNER
     inner_HACK = 1024*1024; // create 1M non-fault-extending stack
#    endif

     if (0)
        VG_(printf)("%#lx 0x%lx  %#lx 0x%lx\n",
                    resvn_start, resvn_size, anon_start, anon_size);

     /* Create a shrinkable reservation followed by an anonymous
        segment.  Together these constitute a growdown stack. */
     SysRes res = VG_(mk_SysRes_Error)(0);
     Bool ok = VG_(am_create_reservation)(
             resvn_start,
             resvn_size - inner_HACK,
             SmUpper,
             anon_size  + inner_HACK
          );
     if (ok) {
        /* allocate a stack - mmap enough space for the stack */
        res = VG_(am_mmap_anon_fixed_client)(
                 anon_start - inner_HACK,
                 anon_size  + inner_HACK,
                 info->stack_prot
              );
     }
     if ((!ok) || sr_isError(res)) {
        /* Allocation of the stack failed.  We have to stop. */
        VG_(printf)("valgrind: "
                    "I failed to allocate space for the application's stack.\n");
        VG_(printf)("valgrind: "
                    "This may be the result of a very large --main-stacksize=\n");
        VG_(printf)("valgrind: setting.  Cannot continue.  Sorry.\n\n");
        VG_(exit)(1);
     }

     vg_assert(ok);
     vg_assert(!sr_isError(res));

     /* Record stack extent -- needed for stack-change code. */
     VG_(clstk_start_base) = anon_start -inner_HACK;
     VG_(clstk_end)  = VG_(clstk_start_base) + anon_size +inner_HACK -1;
   }

   /* ==================== create client stack ==================== */

   Addr* ptr = (Addr*)client_SP;

   /* --- client argc --- */
   Int client_argc = argc + 1;
   *ptr++ = client_argc;

   /* --- client argv --- */
   HChar **client_argv = (HChar **)ptr;
   if (info->interp_name)
      *ptr++ = (Addr)copy_str(&strtab, info->interp_name);
   if (info->interp_args)
      *ptr++ = (Addr)copy_str(&strtab, info->interp_args);

   *ptr++ = (Addr)copy_str(&strtab, VG_(args_the_exename));

   for (Int i = 0; i < VG_(sizeXA)( VG_(args_for_client) ); i++) {
      *ptr++ = (Addr)copy_str(
                       &strtab,
                       * (HChar**) VG_(indexXA)( VG_(args_for_client), i )
                     );
   }
   *ptr++ = 0;

   /* --- envp --- */
   VG_(client_envp) = (HChar **)ptr;
   Int client_envn  = 0;
   for (HChar **cpp = orig_envp; cpp && *cpp; ptr++, cpp++, client_envn++)
      *ptr = (Addr)copy_str(&strtab, *cpp);
   *ptr++ = 0;

   /* --- auxv --- */
   struct auxv *auxv = (struct auxv *)ptr;
   *client_auxv = (UInt *)auxv;
   VG_(client_auxv) = (UWord *)*client_auxv;

   for (; orig_auxv->a_type != AT_NULL; auxv++, orig_auxv++) {

      /* copy the entry... */
      *auxv = *orig_auxv;

      /* ...and fix up / examine the copy */
      switch (auxv->a_type) {
         case AT_IGNORE:
         case AT_PHENT:
         case AT_PAGESZ:
         case AT_FLAGS:
         case AT_EUID:
         case AT_RUID:
         case AT_EGID:
         case AT_RGID:
            /* All these are pointerless, so we don't need to do
             * anything about them. */
            break;

         case AT_PHDR:
            if (info->phdr == 0)
               auxv->a_type = AT_IGNORE;
            else
               auxv->u.a_val = info->phdr;
            break;

         case AT_PHNUM:
            if (info->phdr == 0)
               auxv->a_type = AT_IGNORE;
            else
               auxv->u.a_val = info->phnum;
            break;

         case AT_BASE:
            auxv->u.a_val = info->interp_offset;
            break;

         case AT_ENTRY:
            auxv->u.a_val = info->entry;
            break;

         case AT_STACKBASE:
            auxv->u.a_val = clstack_end;
            break;

         case AT_SUN_EXECNAME:
            /* points to the executable filename */
            auxv->u.a_ptr = copy_str(&strtab, resolved_exe_name);
            break;

         default:
            /* stomp out anything we don't know about */
            VG_(debugLog)(2, "initimg",
                             "stomping auxv entry %llu\n",
                             (ULong)auxv->a_type);
            auxv->a_type = AT_IGNORE;
            break;
      }
   }
   *auxv = *orig_auxv;
   vg_assert(auxv->a_type == AT_NULL);

   /* --- struct ps_strings --- */
   struct vki_ps_strings *pss
      = *client_pss
      = (struct vki_ps_strings*)(clstack_end - sizeof(struct vki_ps_strings));
   pss->ps_argvstr  = client_argv;
   pss->ps_nargvstr = client_argc;
   pss->ps_envstr   = VG_(client_envp);
   pss->ps_nenvstr  = client_envn;

   vg_assert((strtab-stringbase) == stringsize);

   /* client_SP is pointing at client's argc/argv */

   if (0) VG_(printf)("startup SP = %#lx\n", client_SP);
   return client_SP;
}

/* Data segment for brk (heap). It is an expandable anonymous mapping
   abutting a 1-page reservation. The data segment starts at
   VG_(brk_base) and runs up to VG_(brk_limit). None of these two
   values have to be page-aligned. Initial data segment is established
   directly during client program image initialization.

   Notable facts:
   - VG_(brk_base) is not page aligned; does not move
   - VG_(brk_limit) moves between [VG_(brk_base), data segment end]
   - data segment end is always page aligned
   - right after data segment end is 1-page reservation

            |      heap           | 1 page
     +------+------+--------------+-------+
     | BSS  | anon |   anon       | resvn |
     +------+------+--------------+-------+

            ^      ^        ^    ^
            |      |        |    |
            |      |        |    data segment end
            |      |        VG_(brk_limit) -- no alignment constraint
            |      brk_base_pgup -- page aligned
            VG_(brk_base) -- not page aligned -- does not move

   Because VG_(brk_base) is not page-aligned and is initially located within
   pre-established BSS (data) segment, special care has to be taken in the code
   below to handle this feature.

   Reservation segment is used to protect the data segment merging with
   a pre-existing segment. This should be no problem because address space
   manager ensures that requests for client address space are satisfied from
   the highest available addresses. However when memory is low, data segment
   can meet with mmap'ed objects and the reservation segment separates these.
   The page that contains VG_(brk_base) is already allocated by the program's
   loaded data segment. The break syscall wrapper handles this special case. */

/* Establishes initial data segment for brk (heap). */
static Bool setup_client_dataseg(void)
{
   /* Segment size is initially at least 1 MB and at most 8 MB. */
   SizeT m1 = 1024 * 1024;
   SizeT m8 = 8 * m1;
   SizeT initial_size = VG_(client_rlimit_data).rlim_cur;
   VG_(debugLog)(1, "initimg", "Setup client data (brk) segment "
                               "at %#lx\n", VG_(brk_base));
   if (initial_size < m1)
      initial_size = m1;
   if (initial_size > m8)
      initial_size = m8;
   initial_size = VG_PGROUNDUP(initial_size);

   Addr anon_start = VG_PGROUNDUP(VG_(brk_base));
   SizeT anon_size = VG_PGROUNDUP(initial_size);
   Addr resvn_start = anon_start + anon_size;
   SizeT resvn_size = VKI_PAGE_SIZE;

   vg_assert(VG_IS_PAGE_ALIGNED(anon_size));
   vg_assert(VG_IS_PAGE_ALIGNED(resvn_size));
   vg_assert(VG_IS_PAGE_ALIGNED(anon_start));
   vg_assert(VG_IS_PAGE_ALIGNED(resvn_start));
   vg_assert(VG_(brk_base) == VG_(brk_limit));

   /* Find the loaded data segment and remember its protection. */
   const NSegment *seg = VG_(am_find_nsegment)(VG_(brk_base) - 1);
   vg_assert(seg != NULL);
   UInt prot = (seg->hasR ? VKI_PROT_READ : 0)
             | (seg->hasW ? VKI_PROT_WRITE : 0)
             | (seg->hasX ? VKI_PROT_EXEC : 0);

   /* Try to create the data segment and associated reservation where
      VG_(brk_base) says. */
   Bool ok = VG_(am_create_reservation)(resvn_start, resvn_size, SmLower,
                                        anon_size);
   if (!ok) {
      /* That didn't work, we're hosed. */
      return False;
   }

   /* Map the data segment. */
   SysRes sres = VG_(am_mmap_anon_fixed_client)(anon_start, anon_size, prot);
   vg_assert(!sr_isError(sres));
   vg_assert(sr_Res(sres) == anon_start);
   return True;
}

/*====================================================================*/
/*=== TOP-LEVEL: VG_(ii_create_image)                              ===*/
/*====================================================================*/

/* Create the client's initial memory image. */
IIFinaliseImageInfo VG_(ii_create_image)(IICreateImageInfo iicii,
                                         const VexArchInfo *vex_archinfo)
{
   IIFinaliseImageInfo iifii;
   VG_(memset)(&iifii, 0, sizeof(iifii));

   //--------------------------------------------------------------
   // Load client executable, finding in $PATH if necessary
   //   p: get_helprequest_and_toolname()  [for 'exec', 'need_help']
   //   p: layout_remaining_space          [so there's space]
   //--------------------------------------------------------------
   VG_(debugLog)(1, "initimg", "Loading client\n");

   if (VG_(args_the_exename) == NULL)
      VG_(err_missing_prog)();

   ExeInfo info;
   VG_(memset)(&info, 0, sizeof(info));

   HChar resolved_exe_name[VKI_PATH_MAX];
   load_client(&info, resolved_exe_name, sizeof(resolved_exe_name));
   iifii.initial_client_IP = info.init_ip;

   //--------------------------------------------------------------
   // Set up client's environment
   //   p: set-libdir                   [for VG_(libdir)]
   //   p: get_helprequest_and_toolname [for toolname]
   //--------------------------------------------------------------
   VG_(debugLog)(1, "initimg", "Setup client env\n");
   HChar** env = setup_client_env(iicii.envp, iicii.toolname);

   //--------------------------------------------------------------
   // Setup client stack, eip, and VG_(client_arg[cv])
   //   p: load_client()     [for 'info']
   //   p: fix_environment() [for 'env']
   //--------------------------------------------------------------
   {
      /* When allocating space for the client stack, take notice of
       * the --main-stacksize value. This makes it possible to run
       * programs with very large (primary) stack requirements simply
       * by specifying --main-stacksize. */
      /* Logic is as follows:
       * - by default, use the client's current stack rlimit
       * - if that exceeds 16M, clamp to 16M
       * - if a larger --main-stacksize value is specified, use that instead
       * - in all situations, the minimum allowed stack size is 1M
      */
      void* init_sp = iicii.argv - 1;
      SizeT m1  = 1024 * 1024;
      SizeT m16 = 16 * m1;
      SizeT szB = (SizeT)VG_(client_rlimit_stack).rlim_cur;
      if (szB < m1) szB = m1;
      if (szB > m16) szB = m16;
      if (VG_(clo_main_stacksize) > 0) szB = VG_(clo_main_stacksize);
      if (szB < m1) szB = m1;
      szB = VG_PGROUNDUP(szB);
      VG_(debugLog)(1, "initimg",
                       "Setup client stack: size will be %lu\n", szB);

      iifii.clstack_max_size = szB;

      iifii.initial_client_SP
         = setup_client_stack( init_sp, env,
                               &info, &iifii.client_auxv, &iifii.client_pss,
                               iicii.clstack_end, iifii.clstack_max_size,
                               resolved_exe_name );

      VG_(free)(env);

      VG_(debugLog)(2, "initimg",
                       "Client info: "
                       "initial_IP=%p, brk_base=%p\n",
                       (void*)(iifii.initial_client_IP),
                       (void*)VG_(brk_base) );
      VG_(debugLog)(2, "initimg",
                       "Client info: "
                       "initial_SP=%p max_stack_size=%lu\n",
                       (void*)(iifii.initial_client_SP),
                       iifii.clstack_max_size );
   }

   //--------------------------------------------------------------
   // Setup client data (brk) segment.  Initially a 1-page segment
   // which abuts a shrinkable reservation.
   //     p: load_client()     [for 'info' and hence VG_(brk_base)]
   //--------------------------------------------------------------
   {
      if (!setup_client_dataseg()) {
         VG_(printf)("valgrind: cannot initialize data segment (brk).\n");
         VG_(exit)(1);
      }
   }

   VG_(free)(info.interp_name); info.interp_name = NULL;
   VG_(free)(info.interp_args); info.interp_args = NULL;
   return iifii;
}

/*====================================================================*/
/*=== TOP-LEVEL: VG_(finalise_image)                               ===*/
/*====================================================================*/

/* Just before starting the client, we may need to make final
 * adjustments to its initial image.  Also we need to set up the VEX
 * guest state for thread 1 (the root thread) and copy in essential
 * starting values.  This is handed the IIFinaliseImageInfo created by
 * VG_(ii_create_image).
*/
void VG_(ii_finalise_image)( IIFinaliseImageInfo iifii )
{
   ThreadArchState* arch = &VG_(threads)[1].arch;

   /* On NetBSD we get client_{ip/sp/pss}, and start the client with
    * all other registers zeroed. */

#if defined(VGA_amd64)
   vg_assert(0 == sizeof(VexGuestAMD64State) % LibVEX_GUEST_STATE_ALIGN);

   /* Zero out the initial state, and set up the simulated FPU in a
    * sane way. */
   LibVEX_GuestAMD64_initialise(&arch->vex);

   /* Zero out the shadow areas. */
   VG_(memset)(&arch->vex_shadow1, 0, sizeof(VexGuestAMD64State));
   VG_(memset)(&arch->vex_shadow2, 0, sizeof(VexGuestAMD64State));

   /* Put essential stuff into the new state. */
   arch->vex.guest_RSP = iifii.initial_client_SP;
   arch->vex.guest_RIP = iifii.initial_client_IP;
   arch->vex.guest_RBX = (UWord)iifii.client_pss;
   LibVEX_GuestAMD64_put_rflags(VKI_PSL_USERSET, &arch->vex);

#else
#  error Unknown architecture
#endif

#if !defined(PRECISE_GUEST_REG_DEFINEDNESS_AT_STARTUP)
   /* Tell the tool that we just wrote to the registers. */
   VG_TRACK( post_reg_write, Vg_CoreStartup, /*tid*/1, /*offset*/0,
             sizeof(VexGuestArchState));
#endif

   /* Tell the tool about the client data segment and then kill it which will
    * make it inaccessible/unaddressable. */
   const NSegment *seg = VG_(am_find_nsegment)(VG_PGROUNDUP(VG_(brk_base)));
   vg_assert(seg);
   vg_assert(seg->kind == SkAnonC);

   VG_TRACK(new_mem_brk, VG_(brk_base), seg->end + 1 - VG_(brk_base),
            1/*tid*/);
   VG_TRACK(die_mem_brk, VG_(brk_base), seg->end + 1 - VG_(brk_base));
}

#endif // defined(VGO_netbsd)

/*--------------------------------------------------------------------*/
/*--- end                                                          ---*/
/*--------------------------------------------------------------------*/
