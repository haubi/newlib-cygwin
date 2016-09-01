/* dlfcn.cc

This file is part of Cygwin.

This software is a copyrighted work licensed under the terms of the
Cygwin license.  Please consult the file "CYGWIN_LICENSE" for
details. */

#include "winsup.h"
#include <psapi.h>
#include <stdlib.h>
#include <ctype.h>
#include <wctype.h>
#include "path.h"
#include "fhandler.h"
#include "dtable.h"
#include "cygheap.h"
#include "perprocess.h"
#include "dlfcn.h"
#include "cygtls.h"
#include "tls_pbuf.h"
#include "ntdll.h"
#include "shared_info.h"
#include "pathfinder.h"

/* Dumb allocator using memory from tmp_pathbuf.w_get ().

   Does not reuse free'd memory areas.  Instead, memory
   is released when the tmp_pathbuf goes out of scope.

   ATTENTION: Requesting memory from an instance of tmp_pathbuf breaks
   when another instance on a newer stack frame has provided memory. */
class tmp_pathbuf_allocator
  : public allocator_interface
{
  tmp_pathbuf & tp_;
  union
    {
      PWCHAR wideptr;
      void * voidptr;
      char * byteptr;
    }    freemem_;
  size_t freesize_;

  /* allocator_interface */
  virtual void * alloc (size_t need)
  {
    if (need == 0)
      need = 1; /* GNU-ish */
    size_t chunksize = NT_MAX_PATH * sizeof (WCHAR);
    if (need > chunksize)
      api_fatal ("temporary buffer too small for %d bytes", need);
    if (need > freesize_)
      {
	/* skip remaining, use next chunk */
	freemem_.wideptr = tp_.w_get ();
	freesize_ = chunksize;
      }

    void * ret = freemem_.voidptr;

    /* adjust remaining, aligned at 8 byte boundary */
    need = need + 7 - (need - 1) % 8;
    freemem_.byteptr += need;
    if (need > freesize_)
      freesize_ = 0;
    else
      freesize_ -= need;

    return ret;
  }

  /* allocator_interface */
  virtual void free (void *)
  {
    /* no-op: released by tmp_pathbuf at end of scope */
  }

  tmp_pathbuf_allocator ();
  tmp_pathbuf_allocator (tmp_pathbuf_allocator const &);
  tmp_pathbuf_allocator & operator = (tmp_pathbuf_allocator const &);

public:
  /* use tmp_pathbuf of current stack frame */
  tmp_pathbuf_allocator (tmp_pathbuf & tp)
    : allocator_interface ()
    , tp_ (tp)
    , freemem_ ()
    , freesize_ (0)
  {}
};

static void
set_dl_error (const char *str)
{
  strcpy (_my_tls.locals.dl_buffer, strerror (get_errno ()));
  _my_tls.locals.dl_error = 1;
}

/* Identify basename and baselen within name,
   return true if there is a dir in name. */
static bool
spot_basename (const char * &basename, int &baselen, const char * name)
{
  basename = strrchr (name, '/');
  basename = basename ? basename + 1 : name;
  baselen = name + strlen (name) - basename;
  return basename > name;
}

/* Setup basenames using basename and baselen,
   return true if basenames do have some suffix. */
static bool
collect_basenames (pathfinder::basenamelist & basenames,
		   const char * basename, int baselen)
{
  /* like strrchr (basename, '.'), but limited to baselen */
  const char *suffix = basename + baselen;
  while (--suffix >= basename)
    if (*suffix == '.')
      break;

  int suffixlen;
  if (suffix >= basename)
    suffixlen = basename + baselen - suffix;
  else
    {
      suffixlen = 0;
      suffix = NULL;
    }

  char const * ext = "";
  /* Without some suffix, reserve space for a trailing dot to override
     GetModuleHandleExA's automatic adding of the ".dll" suffix. */
  int extlen = suffix ? 0 : 1;

  /* If we have the ".so" suffix, ... */
  if (suffixlen == 3 && !strncmp (suffix, ".so", 3))
    {
      /* ... keep the basename with original suffix, before ... */
      basenames.appendv (basename, baselen, NULL);
      /* ... replacing the ".so" with the ".dll" suffix. */
      baselen -= 3;
      ext = ".dll";
      extlen = 4;
    }
  /* If the basename starts with "lib", ... */
  if (!strncmp (basename, "lib", 3))
    {
      /* ... replace "lib" with "cyg", before ... */
      basenames.appendv ("cyg", 3, basename+3, baselen-3, ext, extlen, NULL);
    }
  /* ... using original basename with new suffix. */
  basenames.appendv (basename, baselen, ext, extlen, NULL);

  return !!suffix;
}

/* Return module handle if one of the basenames is already loaded. */
static void *
find_loaded_basename (pathfinder::basenamelist & basenames,
		      bool have_suffix, DWORD gmheflags)
{
  void * ret = NULL;
  for (pathfinder::basenamelist::buffer_iterator bn (basenames.begin ());
       bn != basenames.end ();
       ++bn)
    {
      char * dot = bn->buffer () + bn->stringlength ();
      if (!have_suffix)
	*dot = '.'; /* do not add the ".dll" suffix */
      GetModuleHandleExA (gmheflags, bn->buffer (), (HMODULE *) &ret);
      if (!have_suffix)
	*dot = '\0'; /* restore */
      if (ret)
	{
	  debug_printf ("at %p: %s", ret, bn->string ());
	  break;
	}
      debug_printf ("not loaded: %s", bn->string ());
    }
  return ret;
}

/* Return module handle if one of the basenames (registered in finder) is
   already loaded from within one of the searchdirs (registered in finder). */
static void *
find_loaded_fullname (pathfinder & finder, bool have_suffix, DWORD gmheflags,
		      path_conv & real_filename, PWCHAR wpathbuf)
{
  void * ret = NULL;

  struct loaded
    : public pathfinder::simple_criterion_interface
  {
    virtual char const * name () const { return "loaded"; }

    bool        have_suffix_;
    DWORD       gmheflags_;
    path_conv & real_filename_;
    PWCHAR      wpathbuf_;
    void *    & ret_;

    loaded (bool have_suffix, DWORD gmheflags,
	    path_conv & real_filename, PWCHAR wpathbuf,
	    void * & ret)
      : pathfinder::simple_criterion_interface ()
      , have_suffix_ (have_suffix)
      , gmheflags_ (gmheflags)
      , real_filename_ (real_filename)
      , wpathbuf_ (wpathbuf)
      , ret_ (ret)
    {}

    /* pathfinder::simple_criterion_interface

       Returns true and provides real_filename & module handle
       if the filename searched by pathfinder is already loaded
       either as the very filename or as the resolved symlink. */
    virtual bool test (const char * filename) const
    {
      /* First, check if we loaded the unresolved symlink's name: */
      int symflag = PC_POSIX;

      for (int i = 0; !ret_ && i < 2; ++i)
	{
	  real_filename_.check (filename, symflag);
	  PWCHAR wpath = real_filename_.get_wide_win32_path (wpathbuf_);
	  if (!wpath)
	    return false;
	  if (!have_suffix_)
	    wcscat (wpath, L"."); /* do not search with a ".dll" suffix */
	  GetModuleHandleExW (gmheflags_, wpath, (HMODULE *) &ret_);
	  if (!real_filename_.issymlink ())
	    break; /* done if no symlink */
	  /* Second, check if we loaded the resolved symlink's name: */
	  symflag |= PC_SYM_FOLLOW;
	}
      return !!ret_;
    }
  };

  finder.find (loaded (have_suffix, gmheflags, real_filename, wpathbuf, ret));

  return ret;
}

/* Identify dir of current executable into exedirbuf using wpathbuf buffer.
   Return length of exedirbuf on success, or zero on error. */
static int
get_exedir (char * exedirbuf, wchar_t * wpathbuf)
{
  /* Unless we have a special cygwin loader, there is no such thing like
     DT_RUNPATH on Windows we can use to search for dlls, except for the
     directory of the main executable. */
  *exedirbuf = '\0';

  wchar_t * wlastsep = wcpcpy (wpathbuf, global_progname);
  /* like wcsrchr(L'\\'), but we know the wcslen already */
  while (--wlastsep > wpathbuf)
    if (*wlastsep == L'\\')
      break;
  if (wlastsep <= wpathbuf)
    return 0;
  *wlastsep = L'\0';

  if (mount_table->conv_to_posix_path (wpathbuf, exedirbuf, 0))
    return 0;

  return strlen (exedirbuf);
}

extern "C" void *
dlopen (const char *name, int flags)
{
  void *ret = NULL;

  do
    {
      if (name == NULL || *name == '\0')
	{
	  ret = (void *) GetModuleHandle (NULL); /* handle for the current module */
	  if (!ret)
	    __seterrno ();
	  break;
	}

      DWORD gmheflags = (flags & RTLD_NODELETE)
		      ?  GET_MODULE_HANDLE_EX_FLAG_PIN
		      : 0;

      tmp_pathbuf tp; /* single one per stack frame */
      tmp_pathbuf_allocator allocator (tp);
      pathfinder::basenamelist basenames (allocator);

      const char *basename;
      int baselen;
      bool have_dir = spot_basename (basename, baselen, name);
      bool have_suffix = collect_basenames (basenames, basename, baselen);

      if (!have_dir)
	{
	  /* search for loaded module without any searchdirs */
	  ret = find_loaded_basename (basenames, have_suffix, gmheflags);
	  if (ret || (flags & RTLD_NOLOAD))
	    break;
	}

      /* handle for the named library */
      path_conv real_filename;
      wchar_t *wpath = tp.w_get ();
      char *cpath = tp.c_get ();

      pathfinder finder (allocator, basenames); /* eats basenames */

      if (have_dir)
	{
	  int dirlen = basename - 1 - name;

	  /* if the specified dir is x/lib, and the current executable
	     dir is x/bin, do the /lib -> /bin mapping, which is the
	     same actually as adding the executable dir */
	  if (dirlen >= 4 && !strncmp (name + dirlen - 4, "/lib", 4))
	    {
	      int exedirlen = get_exedir (cpath, wpath);
	      if (exedirlen == dirlen &&
		  !strncmp (cpath, name, dirlen - 4) &&
		  !strcmp (cpath + dirlen - 4, "/bin"))
		finder.add_searchdir (cpath, exedirlen);
	    }

	  /* search the specified dir */
	  finder.add_searchdir (name, dirlen);

	  /* search for loaded module in the identified searchdirs */
	  ret = find_loaded_fullname (finder, have_suffix, gmheflags,
				      real_filename, wpath);

	  if (ret || (flags & RTLD_NOLOAD))
	    break;
	}
      else
	{
	  /* NOTE: The Windows loader (for linked dlls) does
	     not use the LD_LIBRARY_PATH environment variable. */
	  finder.add_envsearchpath ("LD_LIBRARY_PATH");

	  /* Finally we better have some fallback. */
	  finder.add_searchdir ("/usr/bin", 8);
	  finder.add_searchdir ("/usr/lib", 8);
	}

      /* now search the file system */
      if (!finder.find (pathfinder::
			exists_and_not_dir (real_filename,
					    PC_SYM_FOLLOW | PC_POSIX)))
	{
	  /* If nothing worked, create a relative path from the original
	     incoming filename and let LoadLibrary search for it using the
	     system default DLL search path. */
	  real_filename.check (name, PC_SYM_FOLLOW | PC_NOFULL | PC_NULLEMPTY);
	  if (real_filename.error)
	    break;
	}

      real_filename.get_wide_win32_path (wpath);
      /* Check if the last path component contains a dot.  If so,
	 leave the filename alone.  Otherwise add a trailing dot
	 to override LoadLibrary's automatic adding of a ".dll" suffix. */
      wchar_t *last_bs = wcsrchr (wpath, L'\\') ?: wpath;
      if (last_bs && !wcschr (last_bs, L'.'))
	wcscat (last_bs, L".");

      if (flags & RTLD_NOLOAD)
	{
	  GetModuleHandleExW (gmheflags, wpath, (HMODULE *) &ret);
	  if (ret)
	    break;
	}

#ifndef __x86_64__
      /* Workaround for broken DLLs built against Cygwin versions 1.7.0-49
	 up to 1.7.0-57.  They override the cxx_malloc pointer in their
	 DLL initialization code even if loaded dynamically.  This is a
	 no-no since a later dlclose lets cxx_malloc point into nirvana.
	 The below kludge "fixes" that by reverting the original cxx_malloc
	 pointer after LoadLibrary.  This implies that their overrides
	 won't be applied; that's OK.  All overrides should be present at
	 final link time, as Windows doesn't allow undefined references;
	 it would actually be wrong for a dlopen'd DLL to opportunistically
	 override functions in a way that wasn't known then.  We're not
	 going to try and reproduce the full ELF dynamic loader here!  */

      /* Store original cxx_malloc pointer. */
      struct per_process_cxx_malloc *tmp_malloc;
      tmp_malloc = __cygwin_user_data.cxx_malloc;
#endif

      ret = (void *) LoadLibraryW (wpath);

#ifndef __x86_64__
      /* Restore original cxx_malloc pointer. */
      __cygwin_user_data.cxx_malloc = tmp_malloc;
#endif

      if (ret && gmheflags)
	GetModuleHandleExW (gmheflags, wpath, (HMODULE *) &ret);

      if (!ret)
	__seterrno ();
    }
  while (0);

  if (!ret)
    set_dl_error ("dlopen");
  debug_printf ("ret %p", ret);

  return ret;
}

extern "C" void *
dlsym (void *handle, const char *name)
{
  void *ret = NULL;

  if (handle == RTLD_DEFAULT)
    { /* search all modules */
      PDEBUG_BUFFER buf;
      NTSTATUS status;

      buf = RtlCreateQueryDebugBuffer (0, FALSE);
      if (!buf)
	{
	  set_errno (ENOMEM);
	  set_dl_error ("dlsym");
	  return NULL;
	}
      status = RtlQueryProcessDebugInformation (GetCurrentProcessId (),
						PDI_MODULES, buf);
      if (!NT_SUCCESS (status))
	__seterrno_from_nt_status (status);
      else
	{
	  PDEBUG_MODULE_ARRAY mods = (PDEBUG_MODULE_ARRAY)
				     buf->ModuleInformation;
	  for (ULONG i = 0; i < mods->Count; ++i)
	    if ((ret = (void *)
		       GetProcAddress ((HMODULE) mods->Modules[i].Base, name)))
	      break;
	  if (!ret)
	    set_errno (ENOENT);
	}
      RtlDestroyQueryDebugBuffer (buf);
    }
  else
    {
      ret = (void *) GetProcAddress ((HMODULE) handle, name);
      if (!ret)
	__seterrno ();
    }
  if (!ret)
    set_dl_error ("dlsym");
  debug_printf ("ret %p", ret);
  return ret;
}

extern "C" int
dlclose (void *handle)
{
  int ret;
  if (handle == GetModuleHandle (NULL))
    ret = 0;
  else if (FreeLibrary ((HMODULE) handle))
    ret = 0;
  else
    ret = -1;
  if (ret)
    set_dl_error ("dlclose");
  return ret;
}

extern "C" char *
dlerror ()
{
  char *res;
  if (!_my_tls.locals.dl_error)
    res = NULL;
  else
    {
      _my_tls.locals.dl_error = 0;
      res = _my_tls.locals.dl_buffer;
    }
  return res;
}
