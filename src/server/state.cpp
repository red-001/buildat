// http://www.apache.org/licenses/LICENSE-2.0
// Copyright 2014 Perttu Ahola <celeron55@gmail.com>
#include "state.h"
#include "core/log.h"
#include "rccpp.h"
#include "rccpp_util.h"
#include "config.h"
#include "interface/module.h"
#include "interface/module_info.h"
#include "interface/server.h"
#include "interface/event.h"
#include "interface/file_watch.h"
#include "interface/fs.h"
#include "interface/sha1.h"
#include "interface/mutex.h"
#include "interface/thread_pool.h"
#include "interface/thread.h"
#include "interface/semaphore.h"
#include "interface/debug.h"
#include "interface/select_handler.h"
#include "interface/os.h"
#include <iostream>
#include <algorithm>
#include <fstream>
#include <deque>
#include <list>
#define MODULE "__state"

#ifdef _WIN32
	#define MODULE_EXTENSION "dll"
#else
	#define MODULE_EXTENSION "so"
#endif

extern server::Config g_server_config;
extern bool g_sigint_received;

namespace server {

using interface::Event;

struct ModuleContainer;

struct ModuleThread: public interface::ThreadedThing
{
	ModuleContainer *mc = nullptr;

	ModuleThread(ModuleContainer *mc):
		mc(mc)
	{}

	void run(interface::Thread *thread);
	void on_crash(interface::Thread *thread);

	void handle_direct_cb(
			const std::function<void(interface::Module*)> *direct_cb);
	void handle_event(Event &event);
};

struct ModuleContainer
{
	interface::Server *server;
	interface::ThreadLocalKey *thread_local_key; // Stores mc*
	up_<interface::Module> module;
	interface::ModuleInfo info;
	up_<interface::Thread> thread;
	interface::Mutex mutex; // Protects each of the former variables

	// Allows directly executing code in the module thread
	const std::function<void(interface::Module*)> *direct_cb = nullptr;
	std::exception_ptr direct_cb_exception = nullptr;
	// The actual event queue
	std::deque<Event> event_queue; // Push back, pop front
	// Protects direct_cb and event_queue
	interface::Mutex event_queue_mutex;
	// Counts queued events, and +1 for direct_cb
	interface::Semaphore event_queue_sem;
	// post() when direct_cb has been executed, wait() for that to happen
	interface::Semaphore direct_cb_executed_sem;
	// post() when direct_cb becomes free, wait() for that to happen
	interface::Semaphore direct_cb_free_sem;

	// NOTE: thread-ref_backtraces() Holds the backtraces along the way of a
	// direct callback chain initiated by this module. Cleared when beginning to
	// execute a direct callback. Read when event() (and maybe something else)
	// returns an uncatched exception.

	// Set to true when deleting the module; used for enforcing some limitations
	bool executing_module_destructor = false;

	ModuleContainer(interface::Server *server = nullptr,
			interface::ThreadLocalKey *thread_local_key = NULL,
			interface::Module *module = NULL,
			const interface::ModuleInfo &info = interface::ModuleInfo()):
		server(server),
		thread_local_key(thread_local_key),
		module(module),
		info(info)
	{
		direct_cb_free_sem.post();
	}
	~ModuleContainer(){
		//log_t(MODULE, "M[%s]: Container: Destructing", cs(info.name));
	}
	void init_and_start_thread(){
		{
			interface::MutexScope ms(mutex);
			if(!module)
				throw Exception("init_and_start_thread(): module is null");
			if(info.name != module->m_module_name)
				throw Exception("init_and_start_thread(): Module name does not"
						" match: info.name=\""+info.name+"\","
						" module->m_module_name=\""+module->m_module_name+"\"");
			if(thread != nullptr)
				throw Exception("init_and_start_thread(): thread != nullptr");
			thread.reset(interface::createThread(new ModuleThread(this)));
			thread->set_name(info.name);
			thread->start();
		}
		// Initialize in thread
		std::exception_ptr eptr;
		execute_direct_cb([&](interface::Module *module){
			module->init();
		}, eptr, nullptr);
		if(eptr){
			std::rethrow_exception(eptr);
		}
	}
	void thread_request_stop(){
		interface::MutexScope ms(mutex);
		if(!thread)
			return;
		log_t(MODULE, "M[%s]: Container: Asking thread to exit",
				cs(info.name));
		thread->request_stop();
		// Pretend that direct_cb is now free so that execute_direct_cb can
		// continue (it will cancel due to thread->stop_requested()).
		direct_cb_free_sem.post();
		// Wake up thread so it can exit
		event_queue_sem.post();
		log_t(MODULE, "M[%s]: Container: Asked thread to exit",
				cs(info.name));
	}
	void thread_join(){
		if(thread){
			log_t(MODULE, "M[%s]: Container: Waiting thread to exit",
					cs(info.name));
			thread->join();
			log_t(MODULE, "M[%s]: Container: Thread exited; deleting thread",
					cs(info.name));
			{
				interface::MutexScope ms(mutex);
				thread.reset();
			}
			log_t(MODULE, "M[%s]: Container: Thread exited; thread deleted",
					cs(info.name));
		}
		// Module should have been deleted by the thread. In case the thread
		// failed, delete it here.
		// TODO: This is weird
		module.reset();
	}
	void push_event(const Event &event){
		interface::MutexScope ms(event_queue_mutex);
		event_queue.push_back(event);
		event_queue_sem.post();
	}
	void emit_event_sync(const Event &event){
		interface::MutexScope ms(mutex);
		module->event(event.type, event.p.get());
	}
	// If returns false, the module thread is stopping and cannot be called
	// NOTE: It's not possible for the caller module to be deleted while this is
	//       being executed so a pointer to it is fine (which can be nullptr).
	void execute_direct_cb(const std::function<void(interface::Module*)> &cb,
			std::exception_ptr &result_exception,
			ModuleContainer *caller_mc)
	{
		if(caller_mc == this) // Not allowed
			throw Exception("caller_mc == this");
		log_t(MODULE, "execute_direct_cb[%s]: Waiting for direct_cb to be free",
				cs(info.name));
		direct_cb_free_sem.wait(); // Wait for direct_cb to be free
		{
			interface::MutexScope ms(mutex);
			// This is the last chance to turn around
			if(thread->stop_requested()){
				log_t(MODULE, "execute_direct_cb[%s]: Stop requested; cancelling.",
						cs(info.name));

				// Let the next ones pass too
				direct_cb_free_sem.post();

				// Return an exception to make sure the caller doesn't continue
				// without knowing what it's doing
				ss_ caller_name = caller_mc ? caller_mc->info.name : "__unknown";
				throw interface::TargetModuleNotAvailable(
						"Target module ["+info.name+"] is stopping - "
						"called by ["+caller_name+"]");
			}
		}
		log_t(MODULE, "execute_direct_cb[%s]: Direct_cb is now free. "
				"Waiting for event queue lock", cs(info.name));
		{
			interface::MutexScope ms(event_queue_mutex);
			log_t(MODULE, "execute_direct_cb[%s]: Posting direct_cb",
					cs(info.name));
			direct_cb = &cb;
			direct_cb_exception = nullptr;
			thread->set_caller_thread(interface::Thread::get_current_thread());
			thread->ref_backtraces().clear();
			event_queue_sem.post();
		}
		log_t(MODULE, "execute_direct_cb[%s]: Waiting for execution to finish",
				cs(info.name));
		// NOTE: If execution hangs here, the problem cannot be solved by
		//       forcing this semaphore to open, because exiting this function
		//       while direct_cb is being executed is unsafe. You have to figure
		//       out what direct_cb has ended up waiting for, and fix that.
		// Wait for execution to finish
		direct_cb_executed_sem.wait();
		// Grab execution result
		std::exception_ptr eptr = direct_cb_exception;
		direct_cb_exception = nullptr; // Not to be used anymore
		thread->set_caller_thread(nullptr);
		// Set direct_cb to be free again
		direct_cb_free_sem.post();
		// Handle execution result
		if(eptr){
			log_t(MODULE, "execute_direct_cb[%s]: Execution finished by"
					" exception", cs(info.name));
			/*interface::debug::log_current_backtrace(
					"Backtrace for M["+info.name+"]'s caller:");*/
			result_exception = eptr;
		} else {
			log_t(MODULE, "execute_direct_cb[%s]: Execution finished",
					cs(info.name));
		}
	}
};

void ModuleThread::run(interface::Thread *thread)
{
	mc->thread_local_key->set((void*)mc);

	for(;;){
		// Wait for an event
		mc->event_queue_sem.wait();
		// NOTE: Do not stop here, because we have to process the waited direct
		//       callback or event in order for the caller to be able to safely
		//       return.
		// Grab the direct callback or an event from the queue
		const std::function<void(interface::Module*)> *direct_cb = nullptr;
		Event event;
		bool got_event = false;
		{
			interface::MutexScope ms(mc->event_queue_mutex);
			if(mc->direct_cb){
				direct_cb = mc->direct_cb;
			} else if(!mc->event_queue.empty()){
				event = mc->event_queue.front();
				mc->event_queue.pop_front();
				got_event = true;
			}
		}
		// Check if should stop
		if(thread->stop_requested()){
			log_t(MODULE, "M[%s]: Stopping event loop", cs(mc->info.name));
			// Act like we processed the request
			if(direct_cb){
				log_t(MODULE, "M[%s]: Discarding direct_cb", cs(mc->info.name));
				{
					interface::MutexScope ms(mc->event_queue_mutex);
					mc->direct_cb = nullptr;
				}
				mc->direct_cb_executed_sem.post();
			}
			if(got_event){
				log_t(MODULE, "M[%s]: Discarding event", cs(mc->info.name));
			}
			// Stop
			break;
		}
		if(direct_cb){
			// Handle the direct callback
			handle_direct_cb(direct_cb);
		} else if(got_event){
			// Handle the event
			handle_event(event);
		} else {
			log_w(MODULE, "M[%s]: Event semaphore indicated something happened,"
					" but there was no event, direct callback nor was the thread"
					" asked to stop.", cs(mc->info.name));
		}
	}
	// Delete module in this thread. This is important in case the destruction
	// of some objects in the module is required to be done in the same thread
	// as they were created in.
	// It is also important to delete the module outside of mc->mutex, as doing
	// it in the locked state will only cause deadlocks.
	up_<interface::Module> module_moved;
	{
		interface::MutexScope ms(mc->mutex);
		module_moved = std::move(mc->module);
	}
	mc->executing_module_destructor = true;
	module_moved.reset();
	mc->executing_module_destructor = false;
}

void ModuleThread::on_crash(interface::Thread *thread)
{
	// TODO: Could just restart or something
	mc->server->shutdown(1, "M["+mc->info.name+"] crashed");
}

void ModuleThread::handle_direct_cb(
		const std::function<void(interface::Module*)> *direct_cb)
{
	std::exception_ptr eptr = nullptr;
	if(!mc->module){
		log_w(MODULE, "M[%s]: Module is null; cannot"
				" call direct callback", cs(mc->info.name));
	} else {
		try {
			log_t(MODULE, "M[%s] ~direct_cb(): Executing",
					cs(mc->info.name));
			(*direct_cb)(mc->module.get());
			log_t(MODULE, "M[%s] ~direct_cb(): Executed",
					cs(mc->info.name));
		} catch(...){
			log_t(MODULE, "M[%s] ~direct_cb() failed (exception)",
					cs(mc->info.name));
			// direct_cb() exception should not directly shutdown the
			// server; instead they are passed to the caller. Eventually
			// a caller is reached who isn't using direct_cb(), which
			// then determines the final result of the exception.
			eptr = std::current_exception();

			// If called from another thread
			interface::Thread *current_thread =
					interface::Thread::get_current_thread();
			if(current_thread->get_caller_thread()){
				// Find out the original thread that initiated this direct_cb chain
				interface::Thread *orig_thread =
						current_thread->get_caller_thread();
				while(orig_thread->get_caller_thread()){
					orig_thread = orig_thread->get_caller_thread();
				}

				// Insert exception backtrace to original chain initiator's
				// backtrace list, IF the list is empty
				if(orig_thread->ref_backtraces().empty()){
					interface::debug::ThreadBacktrace bt_step;
					bt_step.thread_name = current_thread->get_name();
					interface::debug::get_exception_backtrace(bt_step.bt);
					orig_thread->ref_backtraces().push_back(bt_step);
				}
			}
		}
	}
	{
		interface::MutexScope ms(mc->event_queue_mutex);
		mc->direct_cb = nullptr;
		mc->direct_cb_exception = eptr;
	}
	mc->direct_cb_executed_sem.post();
}

void ModuleThread::handle_event(Event &event)
{
	if(!mc->module){
		log_w(MODULE, "M[%s]: Module is null; cannot"
				" handle event", cs(mc->info.name));
	} else {
		try {
			log_t(MODULE, "M[%s]->event(): Executing",
					cs(mc->info.name));
			mc->module->event(event.type, event.p.get());
			log_t(MODULE, "M[%s]->event(): Executed",
					cs(mc->info.name));
		} catch(std::exception &e){
			// If event handling results in an uncatched exception, the
			// server shall shut down.
			mc->server->shutdown(1, "M["+mc->info.name+"]->event() "
					"failed: "+e.what());
			log_w(MODULE, "M[%s]->event() failed: %s",
					cs(mc->info.name), e.what());
			if(!mc->thread->ref_backtraces().empty()){
				interface::debug::log_backtrace_chain(
						mc->thread->ref_backtraces(), e.what());
			} else {
				interface::debug::StoredBacktrace bt;
				interface::debug::get_exception_backtrace(bt);
				interface::debug::log_backtrace(bt,
						"Backtrace in M["+mc->info.name+"] for "+
						bt.exception_name+"(\""+e.what()+"\")");
			}
		}
	}
}

struct CState;

struct FileWatchThread: public interface::ThreadedThing
{
	CState *m_server;

	FileWatchThread(CState *server):
		m_server(server)
	{}

	void run(interface::Thread *thread);
	void on_crash(interface::Thread *thread);
};

struct CState: public State, public interface::Server
{
	bool m_shutdown_requested = false;
	int m_shutdown_exit_status = 0;
	ss_ m_shutdown_reason;
	interface::Mutex m_shutdown_mutex;

	up_<rccpp::Compiler> m_compiler;
	ss_ m_modules_path;

	// Thread-local pointer to ModuleContainer of the module of each module
	// thread
	interface::ThreadLocalKey m_thread_local_mc_key;

	sm_<ss_, interface::ModuleInfo> m_module_info; // Info of every seen module
	sm_<ss_, sp_<ModuleContainer>> m_modules; // Currently loaded modules
	set_<ss_> m_unloads_requested;
	sv_<interface::ModuleInfo> m_reloads_requested;
	sm_<ss_, sp_<interface::FileWatch>> m_module_file_watches;
	// Module modifications are accumulated here and core:module_modified events
	// are fired every event loop based on this to lump multiple modifications
	// into one (generally a modification causes many notifications)
	set_<ss_> m_modified_modules; // Module names
	// TODO: Handle properly in reloads (unload by popping from top, then reload
	//       everything until top)
	sv_<ss_> m_module_load_order;
	sv_<sv_<wp_<ModuleContainer>>> m_event_subs;
	// NOTE: You can make a copy of an sp_<ModuleContainer> and unlock this
	//       mutex for processing the module asynchronously (just lock mc->mutex)
	interface::Mutex m_modules_mutex;

	sm_<ss_, ss_> m_tmp_data;
	interface::Mutex m_tmp_data_mutex;

	sm_<ss_, ss_> m_file_paths;
	interface::Mutex m_file_paths_mutex;

	sp_<interface::thread_pool::ThreadPool> m_thread_pool;
	interface::Mutex m_thread_pool_mutex;

	// Must come after the members this will access, which are m_modules_mutex
	// and m_module_file_watches.
	up_<interface::Thread> m_file_watch_thread;

	CState():
		m_compiler(rccpp::createCompiler(
				g_server_config.get<ss_>("compiler_command"))),
		m_thread_pool(interface::thread_pool::createThreadPool())
	{
		m_thread_pool->start(4); // TODO: Configurable

		m_file_watch_thread.reset(interface::createThread(
				new FileWatchThread(this)));
		m_file_watch_thread->set_name("state/select");
		m_file_watch_thread->start();

		// Set basic RCC++ include directories

		// We don't want to directly add the interface path as it contains
		// stuff like mutex.h which match on Windows to Urho3D's Mutex.h
		m_compiler->include_directories.push_back(
				g_server_config.get<ss_>("interface_path")+"/..");
		m_compiler->include_directories.push_back(
				g_server_config.get<ss_>(
				"interface_path")+"/../../3rdparty/cereal/include");
		m_compiler->include_directories.push_back(
				g_server_config.get<ss_>("interface_path")+
				"/../../3rdparty/polyvox/library/PolyVoxCore/include");
		m_compiler->include_directories.push_back(
				g_server_config.get<ss_>("share_path")+"/builtin");

		// Setup Urho3D in RCC++

		sv_<ss_> urho3d_subdirs = {
			"Audio", "Container", "Core", "Engine", "Graphics", "Input", "IO",
			"LuaScript", "Math", "Navigation", "Network", "Physics", "Resource",
			"Scene", "Script", "UI", "Urho2D",
		};
		for(const ss_ &subdir : urho3d_subdirs){
			m_compiler->include_directories.push_back(
					g_server_config.get<ss_>("urho3d_path")+"/Source/Engine/"+subdir);
		}
		m_compiler->include_directories.push_back(
				g_server_config.get<ss_>("urho3d_path")+"/Build/Engine"); // Urho3D.h
		m_compiler->library_directories.push_back(
				g_server_config.get<ss_>("urho3d_path")+"/Lib");
		m_compiler->libraries.push_back("-lUrho3D");
		m_compiler->include_directories.push_back(
				g_server_config.get<ss_>("urho3d_path")+"/Source/ThirdParty/Bullet/src");
	}
	~CState()
	{
	}

	sv_<sp_<ModuleContainer>> get_modules_in_unload_order()
	{
		// Unload modules in reverse load order to make things work more
		// predictably
		sv_<sp_<ModuleContainer>> mcs;
		{
			// Don't have this locked when handling modules because it causes
			// deadlocks
			interface::MutexScope ms(m_modules_mutex);
			for(auto name_it = m_module_load_order.rbegin();
			name_it != m_module_load_order.rend(); ++name_it){
				auto it2 = m_modules.find(*name_it);
				if(it2 == m_modules.end())
					continue;
				sp_<ModuleContainer> &mc = it2->second;
				mcs.push_back(mc);
			}
		}
		return mcs;
	}

	void thread_request_stop()
	{
		m_file_watch_thread->request_stop();

		sv_<sp_<ModuleContainer>> mcs = get_modules_in_unload_order();

		for(sp_<ModuleContainer> &mc : mcs){
			log_t(MODULE, "Requesting module to stop: [%s]", cs(mc->info.name));
			mc->thread_request_stop();
		}
	}

	void thread_join()
	{
		log_v(MODULE, "Waiting: file watch");
		m_file_watch_thread->join();

		sv_<sp_<ModuleContainer>> mcs = get_modules_in_unload_order();

		// Wait for threads to stop and delete module container references
		log_v(MODULE, "Waiting: modules");
		for(sp_<ModuleContainer> &mc : mcs){
			log_d(MODULE, "Waiting for module to stop: [%s]", cs(mc->info.name));
			mc->thread_join();
			// Remove our reference to the module container, so that any child
			// threads it will now delete will not get deadlocked in trying to
			// access the module
			// (This is a shared pointer)
			mc.reset();
		}
	}

	void shutdown(int exit_status, const ss_ &reason)
	{
		interface::MutexScope ms(m_shutdown_mutex);
		if(m_shutdown_requested && exit_status == 0){
			// Only reset these values for exit values indicating failure
			return;
		}
		log_i(MODULE, "Server shutdown requested; exit_status=%i, reason=\"%s\"",
				exit_status, cs(reason));
		m_shutdown_requested = true;
		m_shutdown_exit_status = exit_status;
		m_shutdown_reason = reason;
	}

	bool is_shutdown_requested(int *exit_status = nullptr, ss_ *reason = nullptr)
	{
		interface::MutexScope ms(m_shutdown_mutex);
		if(m_shutdown_requested){
			if(exit_status)
				*exit_status = m_shutdown_exit_status;
			if(reason)
				*reason = m_shutdown_reason;
		}
		return m_shutdown_requested;
	}

	interface::Module* build_module_u(const interface::ModuleInfo &info)
	{
		ss_ init_cpp_path = info.path+"/"+info.name+".cpp";

		// Set up file watch

		sv_<ss_> files_to_watch = {init_cpp_path};
		sv_<ss_> include_dirs = m_compiler->include_directories;
		include_dirs.push_back(m_modules_path);
		sv_<ss_> includes = list_includes(init_cpp_path, include_dirs);
		log_d(MODULE, "Includes: %s", cs(dump(includes)));
		files_to_watch.insert(files_to_watch.end(), includes.begin(),
				includes.end());

		if(m_module_file_watches.count(info.name) == 0){
			sp_<interface::FileWatch> w(interface::createFileWatch());
			for(const ss_ &watch_path : files_to_watch){
				ss_ dir_path = interface::fs::strip_file_name(watch_path);
				w->add(dir_path, [this, info, watch_path](const ss_ &modified_path){
					if(modified_path != watch_path)
						return;
					log_i(MODULE, "Module modified: %s: %s",
							cs(info.name), cs(info.path));
					m_modified_modules.insert(info.name);
				});
			}
			m_module_file_watches[info.name] = w;
		}

		// Build

		ss_ extra_cxxflags = info.meta.cxxflags;
		ss_ extra_ldflags = info.meta.ldflags;
#ifdef _WIN32
		extra_cxxflags += " "+info.meta.cxxflags_windows;
		extra_ldflags += " "+info.meta.ldflags_windows;
		// Needed for every module
		extra_ldflags += " -lbuildat_core";
		// Always include these to make life easier
		extra_ldflags += " -lwsock32 -lws2_32";
		// Add the path of the current executable to the library search path
		{
			ss_ exe_path = interface::os::get_current_exe_path();
			ss_ exe_dir = interface::fs::strip_file_name(exe_path);
			extra_ldflags += " -L\""+exe_dir+"\"";
		}
#else
		extra_cxxflags += " "+info.meta.cxxflags_linux;
		extra_ldflags += " "+info.meta.ldflags_linux;
#endif
		log_d(MODULE, "extra_cxxflags: %s", cs(extra_cxxflags));
		log_d(MODULE, "extra_ldflags: %s", cs(extra_ldflags));

		bool skip_compile = g_server_config.get<json::Value>(
				"skip_compiling_modules").get(info.name).as_boolean();

		sv_<ss_> files_to_hash = {init_cpp_path};
		files_to_hash.insert(
				files_to_hash.begin(), includes.begin(), includes.end());
		ss_ content_hash = hash_files(files_to_hash);
		log_d(MODULE, "Module hash: %s", cs(interface::sha1::hex(content_hash)));

#ifdef _WIN32
		// On Windows, we need a new name for each modification of the module
		// because Windows caches DLLs by name
		ss_ build_dst = g_server_config.get<ss_>("rccpp_build_path") +
				"/"+info.name+"_"+interface::sha1::hex(content_hash)+"."+
				MODULE_EXTENSION;
		// TODO: Delete old ones
#else
		ss_ build_dst = g_server_config.get<ss_>("rccpp_build_path") +
				"/"+info.name+"."+MODULE_EXTENSION;
#endif

		ss_ hashfile_path = build_dst+".hash";

		if(!skip_compile){
			if(!std::ifstream(build_dst).good()){
				// Result file does not exist at all, no need to check hashes
			} else {
				ss_ previous_hash;
				{
					std::ifstream f(hashfile_path);
					if(f.good()){
						previous_hash = ss_((std::istreambuf_iterator<char>(f)),
								std::istreambuf_iterator<char>());
					}
				}
				if(previous_hash == content_hash){
					log_v(MODULE, "No need to recompile %s", cs(info.name));
					skip_compile = true;
				}
			}
		}

		m_compiler->include_directories.push_back(m_modules_path);
		bool build_ok = m_compiler->build(info.name, init_cpp_path, build_dst,
				extra_cxxflags, extra_ldflags, skip_compile);
		m_compiler->include_directories.pop_back();

		if(!build_ok){
			log_w(MODULE, "Failed to build module %s", cs(info.name));
			return nullptr;
		}

		// Update hash file
		if(!skip_compile){
			std::ofstream f(hashfile_path);
			f<<content_hash;
		}

		// Construct instance

		interface::Module *m = static_cast<interface::Module*>(
				m_compiler->construct(info.name.c_str(), this));
		return m;
	}

	// Can be used for loading hardcoded modules.
	// There intentionally is no core:module_loaded event.
	void load_module_direct_u(interface::Module *m, const ss_ &name)
	{
		sp_<ModuleContainer> mc;
		{
			interface::MutexScope ms(m_modules_mutex);

			interface::ModuleInfo info;
			info.name = name;
			info.path = "";

			log_i(MODULE, "Loading module %s (hardcoded)", cs(info.name));

			m_module_info[info.name] = info;

			// TODO: Fix to something like in load_module()
			mc = sp_<ModuleContainer>(new ModuleContainer(
					this, &m_thread_local_mc_key, m, info));
			m_modules[info.name] = mc;
			m_module_load_order.push_back(info.name);
		}

		// Call init() and start thread
		mc->init_and_start_thread();
	}

	bool load_module(const interface::ModuleInfo &info)
	{
		interface::Module *m = nullptr;
		{
			interface::MutexScope ms(m_modules_mutex);

			if(m_modules.find(info.name) != m_modules.end()){
				log_w(MODULE, "Cannot load module %s from %s: Already loaded",
						cs(info.name), cs(info.path));
				return false;
			}

			log_i(MODULE, "Loading module %s from %s", cs(info.name), cs(info.path));

			m_module_info[info.name] = info;

			if(!info.meta.disable_cpp){
				m = build_module_u(info);

				if(m == nullptr){
					log_w(MODULE, "Failed to construct module %s instance",
							cs(info.name));
					return false;
				}
			}
		}

		sp_<ModuleContainer> mc = sp_<ModuleContainer>(
				new ModuleContainer(this, &m_thread_local_mc_key, m, info));

		{
			interface::MutexScope ms(m_modules_mutex);

			m_modules[info.name] = mc;
			m_module_load_order.push_back(info.name);
		}

		if(!info.meta.disable_cpp){
			// Call init() and start thread
			mc->init_and_start_thread();
		}

		emit_event(Event("core:module_loaded",
				new interface::ModuleLoadedEvent(info.name)));
		return true;
	}

	void load_modules(const ss_ &path)
	{
		m_modules_path = path;

		interface::ModuleInfo info;
		info.name = "__loader";
		info.path = path+"/"+info.name;

		if(!load_module(info)){
			shutdown(1, "Failed to load __loader module");
			return;
		}

		// Allow loader to load other modules.
		// Emit synchronously because threading doesn't matter at this point in
		// initialization and we have to wait for it to complete.
		emit_event(Event("core:load_modules"), true);

		if(is_shutdown_requested())
			return;

		// Now that everyone is listening, we can fire the start event
		emit_event(Event("core:start"));
	}

	// interface::Server version; doesn't directly unload
	void unload_module(const ss_ &module_name)
	{
		log_v(MODULE, "unload_module(%s)", cs(module_name));
		interface::MutexScope ms(m_modules_mutex);
		auto it = m_modules.find(module_name);
		if(it == m_modules.end()){
			log_w(MODULE, "unload_module(%s): Not loaded", cs(module_name));
			return;
		}
		m_unloads_requested.insert(module_name);
	}

	void reload_module(const interface::ModuleInfo &info)
	{
		log_i(MODULE, "reload_module(%s)", cs(info.name));
		interface::MutexScope ms(m_modules_mutex);
		for(interface::ModuleInfo &info0 : m_reloads_requested){
			if(info0.name == info.name){
				info0 = info; // Update existing request
				return;
			}
		}
		m_reloads_requested.push_back(info);
	}

	void reload_module(const ss_ &module_name)
	{
		interface::ModuleInfo info;
		{
			interface::MutexScope ms(m_modules_mutex);
			auto it = m_module_info.find(module_name);
			if(it == m_module_info.end()){
				log_w(MODULE, "reload_module: Module info not found: %s",
						cs(module_name));
				return;
			}
			info = it->second;
		}
		reload_module(info);
	}

	// Direct version; internal and unsafe
	// Call with no mutexes locked.
	void unload_module_u(const ss_ &module_name)
	{
		log_i(MODULE, "unload_module_u(): module_name=%s", cs(module_name));
		sp_<ModuleContainer> mc;
		{
			interface::MutexScope ms(m_modules_mutex);
			// Get and lock module
			auto it = m_modules.find(module_name);
			if(it == m_modules.end()){
				log_w(MODULE, "unload_module_u(): Module not found: %s",
						cs(module_name));
				return;
			}
			mc = it->second;
			{
				interface::MutexScope mc_ms(mc->mutex);
				// Delete subscriptions
				log_t(MODULE, "unload_module_u[%s]: Deleting subscriptions",
						cs(module_name));
				{
					for(Event::Type type = 0; type < m_event_subs.size(); type++){
						sv_<wp_<ModuleContainer>> &sublist = m_event_subs[type];
						sv_<wp_<ModuleContainer>> new_sublist;
						for(wp_<ModuleContainer> &mc1 : sublist){
							if(sp_<ModuleContainer>(mc1.lock()).get() !=
									mc.get())
								new_sublist.push_back(mc1);
							else
								log_v(MODULE,
										"Removing %s subscription to event %zu",
										cs(module_name), type);
						}
						sublist = new_sublist;
					}
				}
				// Remove server-wide reference to module container
				m_modules.erase(module_name);
			}
		}

		// Destruct module
		log_t(MODULE, "unload_module_u[%s]: Deleting module", cs(module_name));
		mc->thread_request_stop();
		mc->thread_join();

		{
			interface::MutexScope ms(m_modules_mutex);
			// So, hopefully this is the last reference because we're going to
			// unload the shared executable...
			if(!mc.unique())
				log_w(MODULE, "unload_module_u[%s]: This is not the last container"
						" reference; unloading shared executable is probably unsafe",
						cs(module_name));
			// Drop reference to container
			log_t(MODULE, "unload_module_u[%s]: Dropping container",
					cs(module_name));
			mc.reset();
			// Unload shared executable
			log_t(MODULE, "unload_module_u[%s]: Unloading shared executable",
					cs(module_name));
			m_compiler->unload(module_name);

			emit_event(Event("core:module_unloaded",
					new interface::ModuleUnloadedEvent(module_name)));
		}
	}

	ss_ get_modules_path()
	{
		return m_modules_path;
	}

	ss_ get_builtin_modules_path()
	{
		return g_server_config.get<ss_>("share_path")+"/builtin";
	}

	ss_ get_module_path(const ss_ &module_name)
	{
		interface::MutexScope ms(m_modules_mutex);
		auto it = m_modules.find(module_name);
		if(it == m_modules.end())
			throw ModuleNotFoundException(ss_()+"Module not found: "+module_name);
		ModuleContainer *mc = it->second.get();
		return mc->info.path;
	}

	interface::Module* get_module(const ss_ &module_name)
	{
		interface::MutexScope ms(m_modules_mutex);
		auto it = m_modules.find(module_name);
		if(it == m_modules.end())
			return NULL;
		return it->second->module.get();
	}

	interface::Module* check_module(const ss_ &module_name)
	{
		interface::Module *m = get_module(module_name);
		if(m) return m;
		throw ModuleNotFoundException(ss_()+"Module not found: "+module_name);
	}

	bool has_module(const ss_ &module_name)
	{
		interface::MutexScope ms(m_modules_mutex);
		auto it = m_modules.find(module_name);
		return (it != m_modules.end());
	}

	sv_<ss_> get_loaded_modules()
	{
		interface::MutexScope ms(m_modules_mutex);
		sv_<ss_> result;
		for(auto &pair : m_modules){
			result.push_back(pair.first);
		}
		return result;
	}

	// Call with m_modules_mutex locked
	bool is_dependency_u(ModuleContainer *mc_should_be_dependent,
			ModuleContainer *mc_should_be_dependency)
	{
		const ss_ &search_dep_name = mc_should_be_dependency->info.name;
		const interface::ModuleInfo &info = mc_should_be_dependent->info;
		// Breadth-first
		for(const interface::ModuleDependency &dep : info.meta.dependencies){
			/*log_t(MODULE, "is_dependency_u(): \"%s\" has dependency \"%s\"; "
					"searching for \"%s\"",
					cs(info.name), cs(dep.module), cs(search_dep_name));*/
			if(dep.module == search_dep_name)
				return true;
		}
		for(const interface::ModuleDependency &dep : info.meta.dependencies){
			auto it = m_modules.find(dep.module);
			if(it == m_modules.end())
				continue;
			ModuleContainer *mc_dependency = it->second.get();
			bool is = is_dependency_u(mc_dependency, mc_should_be_dependency);
			if(is)
				return true;
		}
		/*log_t(MODULE, "is_dependency_u(): \"%s\" does not depend on \"%s\"",
				cs(info.name), cs(search_dep_name));*/
		return false;
	}

	// Throws on invalid access
	// Call with m_modules_mutex locked
	void check_valid_access_u(
			ModuleContainer *target_mc,
			ModuleContainer *caller_mc
			){
		const ss_ &target_name = target_mc->info.name;
		const ss_ &caller_name = caller_mc->info.name;

		// Access is invalid if target is caller
		if(caller_mc == target_mc)
			throw Exception("Cannot access \""+target_name+"\" from \""+
					caller_name+"\": Accessing itself is disallowed");

		// Access is invalid if caller is a direct or indirect dependency of
		// target
		if(is_dependency_u(target_mc, caller_mc))
			throw Exception("Cannot access \""+target_name+"\" from \""+
					caller_name+"\": Target depends on caller - access must "
					"happen the other way around");

		// The thing we are trying to disallow is that if module 1 accesses
		// module 2 at some point, then at no point shall module 2 be allowed to
		// access module 1.

		// Access is valid
	}

	bool access_module(const ss_ &module_name,
			std::function<void(interface::Module*)> cb)
	{
		ModuleContainer *caller_mc =
				(ModuleContainer*)m_thread_local_mc_key.get();

		try {
			sp_<ModuleContainer> mc;
			{
				interface::MutexScope ms(m_modules_mutex);

				auto it = m_modules.find(module_name);
				if(it == m_modules.end())
					throw Exception("access_module(): Module \""+module_name+
							"\" not found");
				mc = it->second;
				if(!mc)
					throw Exception("access_module(): Module \""+module_name+
							"\" container is null");

				if(caller_mc){
					log_t(MODULE, "access_module[%s]: Called by \"%s\"",
							cs(mc->info.name), cs(caller_mc->info.name));

					// Throws exception if not valid.
					// If accessing a module from a nested access_module(), this
					// function is called from the thread of the nested module,
					// effectively taking into account the lock hierarchy.
					check_valid_access_u(mc.get(), caller_mc);
				} else {
					log_t(MODULE, "access_module[%s]: Called by something else"
							" than a module", cs(mc->info.name));
				}
			}

			// Execute callback in module thread
			std::exception_ptr eptr;
			mc->execute_direct_cb(cb, eptr, caller_mc);
			if(eptr){
				interface::Thread *current_thread =
						interface::Thread::get_current_thread();

				// If not being called by a thread, there's nowhere we can store the
				// backtrace (and it wouldn't make sense anyway as there is no
				// callback chain)
				if(current_thread == nullptr){
					std::rethrow_exception(eptr);
				}

				// NOTE: In each Thread there is a pointer to the Thread that is
				//       currently doing a direct call, or nullptr if a direct call
				//       is not being executed.
				// NOTE: The parent callers in the chain cannot be deleted while
				//       this function is executing so we can freely access them.

				// Find out the original thread that initiated this direct_cb chain
				interface::Thread *orig_thread = current_thread;
				while(orig_thread->get_caller_thread()){
					orig_thread = orig_thread->get_caller_thread();
				}

				// Insert backtrace to original chain initiator's backtrace list
				interface::debug::ThreadBacktrace bt_step;
				bt_step.thread_name = current_thread->get_name();
				interface::debug::get_current_backtrace(bt_step.bt);
				orig_thread->ref_backtraces().push_back(bt_step);

				// NOTE: When an exception comes uncatched from module->event(), the
				//       direct_cb backtrace stack can be logged after the backtrace
				//       gotten from the __cxa_throw catch. The backtrace catched by
				//       the __cxa_throw wrapper is from the furthermost thread in
				//       the direct_cb chain, from which the event was just
				//       propagated downwards (while recording the other backtraces
				//       like specified here).

				// Re-throw the exception so that the chain gets unwinded (while we
				// collect backtraces at each step)
				std::rethrow_exception(eptr);
			}
		} catch(...){
			std::exception_ptr eptr = std::current_exception();
			// If a destructor doesn't catch an exception, the whole program
			// will abort. So, do not pass exception to destructor.
			if(caller_mc && caller_mc->executing_module_destructor){
				try {
					std::rethrow_exception(eptr);
				} catch(std::exception &e){
					log_w(MODULE, "access_module[%s]: Ignoring exception in"
							" [%s] destructor: \"%s\"", cs(module_name),
							cs(caller_mc->info.name), e.what());
				} catch(...){
					log_w(MODULE, "access_module[%s]: Ignoring exception in"
							" [%s] destructor", cs(module_name),
							cs(caller_mc->info.name));
				}
				return true;
			}
			// Pass exception to caller normally
			std::rethrow_exception(eptr);
		}
		return true;
	}

	void sub_event(struct interface::Module *module,
			const Event::Type &type)
	{
		// Lock modules so that the subscribing one isn't removed asynchronously
		interface::MutexScope ms(m_modules_mutex);
		// Make sure module is a known instance
		sp_<ModuleContainer> mc0;
		ss_ module_name = "(unknown)";
		for(auto &pair : m_modules){
			sp_<ModuleContainer> &mc = pair.second;
			if(mc->module.get() == module){
				mc0 = mc;
				module_name = pair.first;
				break;
			}
		}
		if(mc0 == nullptr){
			log_w(MODULE, "sub_event(): Not a known module");
			return;
		}
		if(m_event_subs.size() <= type + 1)
			m_event_subs.resize(type + 1);
		sv_<wp_<ModuleContainer>> &sublist = m_event_subs[type];
		bool found = false;
		for(wp_<ModuleContainer> &item : sublist){
			if(item.lock() == mc0){
				found = true;
				break;
			}
		}
		if(found){
			log_w(MODULE, "sub_event(): Already on list: %s", cs(module_name));
			return;
		}
		auto *evreg = interface::getGlobalEventRegistry();
		log_d(MODULE, "sub_event(): %s subscribed to %s (%zu)",
				cs(module_name), cs(evreg->name(type)), type);
		sublist.push_back(wp_<ModuleContainer>(mc0));
	}

	// Do not use synchronous=true unless specifically needed in a special case.
	void emit_event(Event event, bool synchronous)
	{
		if(log_get_max_level() >= CORE_TRACE){
			auto *evreg = interface::getGlobalEventRegistry();
			log_t(MODULE, "emit_event(): %s (%zu)",
					cs(evreg->name(event.type)), event.type);
		}

		sv_<sv_<wp_<ModuleContainer>>> event_subs_snapshot;
		{
			interface::MutexScope ms(m_modules_mutex);
			event_subs_snapshot = m_event_subs;
		}

		if(event.type >= event_subs_snapshot.size()){
			log_t(MODULE, "emit_event(): %zu: No subs", event.type);
			return;
		}
		sv_<wp_<ModuleContainer>> &sublist = event_subs_snapshot[event.type];
		if(sublist.empty()){
			log_t(MODULE, "emit_event(): %zu: No subs", event.type);
			return;
		}
		if(log_get_max_level() >= CORE_TRACE){
			auto *evreg = interface::getGlobalEventRegistry();
			log_t(MODULE, "emit_event(): %s (%zu): Pushing to %zu modules",
					cs(evreg->name(event.type)), event.type, sublist.size());
		}
		for(wp_<ModuleContainer> &mc_weak : sublist){
			sp_<ModuleContainer> mc(mc_weak.lock());
			if(mc){
				if(synchronous)
					mc->emit_event_sync(event);
				else
					mc->push_event(event);
			} else {
				auto *evreg = interface::getGlobalEventRegistry();
				log_t(MODULE, "emit_event(): %s: (%zu): Subscriber weak pointer"
						" is null", cs(evreg->name(event.type)), event.type);
			}
		}
	}

	void emit_event(Event event)
	{
		emit_event(event, false);
	}

	void handle_events()
	{
		// Get modified modules and push events to queue
		{
			interface::MutexScope ms(m_modules_mutex);
			set_<ss_> modified_modules;
			modified_modules.swap(m_modified_modules);
			for(const ss_ &name : modified_modules){
				auto it = m_module_info.find(name);
				if(it == m_module_info.end())
					throw Exception("Info of modified module not available");
				interface::ModuleInfo &info = it->second;
				emit_event(Event("core:module_modified",
						new interface::ModuleModifiedEvent(
						info.name, info.path)));
			}
		}

		// Handle module unloads and reloads as requested
		handle_unloads_and_reloads();
	}

	void handle_unloads_and_reloads()
	{
		// Grab unload and reload requests into unload and load queues
		sv_<ss_> unloads_requested;
		sv_<interface::ModuleInfo> loads_requested;
		{
			interface::MutexScope ms(m_modules_mutex);

			for(const ss_ &module_name : m_unloads_requested){
				unloads_requested.push_back(module_name);
			}
			m_unloads_requested.clear();

			for(const interface::ModuleInfo &info : m_reloads_requested){
				unloads_requested.push_back(info.name);
				loads_requested.push_back(info);
			}
			m_reloads_requested.clear();
		}
		// Send core:unload events synchronously to modules
		for(const ss_ &module_name : unloads_requested){
			log_t(MODULE, "reload[%s]: Synchronous core:unload", cs(module_name));
			access_module(module_name, [&](interface::Module *module){
				module->event(Event::t("core:unload"), nullptr);
			});
		}
		// Unload modules
		for(const ss_ &module_name : unloads_requested){
			log_i(MODULE, "Unloading %s", cs(module_name));
			unload_module_u(module_name);
		}
		// Load modules
		for(const interface::ModuleInfo &info : loads_requested){
			log_i(MODULE, "Loading %s (reload requested)", cs(info.name));
			// Load module
			load_module(info);
			// Send core:continue synchronously to module
			access_module(info.name, [&](interface::Module *module){
				module->event(Event::t("core:continue"), nullptr);
			});
		}
	}

	void tmp_store_data(const ss_ &name, const ss_ &data)
	{
		interface::MutexScope ms(m_tmp_data_mutex);
		m_tmp_data[name] = data;
	}

	ss_ tmp_restore_data(const ss_ &name)
	{
		interface::MutexScope ms(m_tmp_data_mutex);
		ss_ data = m_tmp_data[name];
		m_tmp_data.erase(name);
		return data;
	}

	// Add resource file path (to make a mirror of the client)
	void add_file_path(const ss_ &name, const ss_ &path)
	{
		log_d(MODULE, "add_file_path(): %s -> %s", cs(name), cs(path));
		interface::MutexScope ms(m_file_paths_mutex);
		m_file_paths[name] = path;
	}

	// Returns "" if not found
	ss_ get_file_path(const ss_ &name)
	{
		interface::MutexScope ms(m_file_paths_mutex);
		auto it = m_file_paths.find(name);
		if(it == m_file_paths.end())
			return "";
		return it->second;
	}

	const interface::ServerConfig& get_config()
	{
		return g_server_config;
	}

	void access_thread_pool(std::function<void(
			interface::thread_pool::ThreadPool*pool)> cb)
	{
		interface::MutexScope ms(m_thread_pool_mutex);
		cb(m_thread_pool.get());
	}
};

void FileWatchThread::run(interface::Thread *thread)
{
	interface::SelectHandler handler;

	while(!thread->stop_requested()){
		sv_<int> sockets;
		{
			interface::MutexScope ms(m_server->m_modules_mutex);
			for(auto &pair : m_server->m_module_file_watches){
				sv_<int> fds = pair.second->get_fds();
				sockets.insert(sockets.begin(), fds.begin(), fds.end());
			}
		}

		sv_<int> active_sockets;
		bool ok = handler.check(500000, sockets, active_sockets);
		(void)ok; // Unused

		if(active_sockets.empty())
			continue;

		{
			interface::MutexScope ms(m_server->m_modules_mutex);
			for(auto &pair : m_server->m_module_file_watches){
				for(int fd : active_sockets){
					pair.second->report_fd(fd);
				}
			}
		}
	}
}

void FileWatchThread::on_crash(interface::Thread *thread)
{
	m_server->shutdown(1, "FileWatchThread crashed");
}

State* createState()
{
	return new CState();
}
}
// vim: set noet ts=4 sw=4:
