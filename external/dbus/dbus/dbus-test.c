
/* -*- mode: C; c-file-style: "gnu" -*- */

#include <config.h>
#include "dbus-test.h"
#include "dbus-sysdeps.h"
#include "dbus-internals.h"
#include <stdio.h>
#include <stdlib.h>

#ifdef DBUS_BUILD_TESTS
static void
die (const char *failure)
{
  fprintf (stderr, "Unit test failed: %s\n", failure);
  exit (1);
}

static void
check_memleaks (void)
{
  dbus_shutdown ();

  printf ("%s: checking for memleaks\n", "dbus-test");
  if (_dbus_get_malloc_blocks_outstanding () != 0)
    {
      _dbus_warn ("%d dbus_malloc blocks were not freed\n",
                  _dbus_get_malloc_blocks_outstanding ());
      die ("memleaks");
    }
}

typedef dbus_bool_t (*TestFunc)(void);
typedef dbus_bool_t (*TestDataFunc)(const char *data);

static void
run_test (const char             *test_name,
	  const char             *specific_test,
	  TestFunc                test)
{
  if (!specific_test || strcmp (specific_test, test_name) == 0)
    {
      printf ("%s: running %s tests\n", "dbus-test", test_name);
      if (!test ())
	die (test_name);
    }

  check_memleaks ();
}

static void
run_data_test (const char             *test_name,
	       const char             *specific_test,
	       TestDataFunc            test,
	       const char             *test_data_dir)
{
  if (!specific_test || strcmp (specific_test, test_name) == 0)
    {
      printf ("%s: running %s tests\n", "dbus-test", test_name);
      if (!test (test_data_dir))
	die (test_name);
    }

  check_memleaks ();
}

#endif /* DBUS_BUILD_TESTS */

void
dbus_internal_do_not_use_run_tests (const char *test_data_dir, const char *specific_test)
{
#ifdef DBUS_BUILD_TESTS
  if (!_dbus_threads_init_debug ())
    die ("debug threads init");
  
  if (test_data_dir == NULL)
    test_data_dir = _dbus_getenv ("DBUS_TEST_DATA");

  if (test_data_dir != NULL)
    printf ("Test data in %s\n", test_data_dir);
  else
    printf ("No test data!\n");

  run_test ("string", specific_test, _dbus_string_test);
  
  run_test ("sysdeps", specific_test, _dbus_sysdeps_test);
  
  run_test ("data-slot", specific_test, _dbus_data_slot_test);

  run_test ("misc", specific_test, _dbus_misc_test);
  
  run_test ("address", specific_test, _dbus_address_test);

  run_test ("server", specific_test, _dbus_server_test);

  run_test ("object-tree", specific_test, _dbus_object_tree_test);

  run_test ("signature", specific_test, _dbus_signature_test);
  
  run_test ("marshalling", specific_test, _dbus_marshal_test);

#if 0
  printf ("%s: running recursive marshalling tests\n", "dbus-test");
  if (!_dbus_marshal_recursive_test ())
    die ("recursive marshal");

  check_memleaks ();
#else
  _dbus_warn ("recursive marshal tests disabled\n");
#endif

  run_test ("byteswap", specific_test, _dbus_marshal_byteswap_test);
  
  run_test ("memory", specific_test, _dbus_memory_test);

#if 1
  run_test ("mem-pool", specific_test, _dbus_mem_pool_test);
#endif
  
  run_test ("list", specific_test, _dbus_list_test);

  run_test ("marshal-validate", specific_test, _dbus_marshal_validate_test);

  run_test ("marshal-header", specific_test, _dbus_marshal_header_test);
  
  run_data_test ("message", specific_test, _dbus_message_test, test_data_dir);
  
  run_test ("hash", specific_test, _dbus_hash_test);

  run_data_test ("spawn", specific_test, _dbus_spawn_test, test_data_dir);
  
  run_data_test ("userdb", specific_test, _dbus_userdb_test, test_data_dir);
  
  run_test ("keyring", specific_test, _dbus_keyring_test);
  
#if 0
  printf ("%s: running md5 tests\n", "dbus-test");
  if (!_dbus_md5_test ())
    die ("md5");

  check_memleaks ();
#endif
  
  run_data_test ("sha", specific_test, _dbus_sha_test, test_data_dir);
  
  run_data_test ("auth", specific_test, _dbus_auth_test, test_data_dir);

  run_data_test ("pending-call", specific_test, _dbus_pending_call_test, test_data_dir);
  
  printf ("%s: completed successfully\n", "dbus-test");
#else
  printf ("Not compiled with unit tests, not running any\n");
#endif
}

