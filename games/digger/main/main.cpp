#include "core/log.h"
#include "client_file/api.h"
#include "network/api.h"
#include "replicate/api.h"
#include "interface/module.h"
#include "interface/server.h"
#include "interface/event.h"
#include "interface/mesh.h"
#include "interface/voxel.h"
#include <Scene.h>
#include <RigidBody.h>
#include <CollisionShape.h>
#include <ResourceCache.h>
#include <Context.h>
#include <StaticModel.h>
#include <Model.h>
#include <Material.h>
#include <Texture2D.h>
#include <Technique.h>
#include <cereal/archives/portable_binary.hpp>
#include <cereal/types/unordered_map.hpp>
#include <cereal/types/vector.hpp>

namespace digger {

using interface::Event;
using namespace Urho3D;
namespace magic = Urho3D;

struct Module: public interface::Module
{
	interface::Server *m_server;
	uint m_slow_count = 0;
	sp_<interface::TextureAtlasRegistry> m_atlas_reg;
	sp_<interface::VoxelRegistry> m_voxel_reg;

	Module(interface::Server *server):
		interface::Module("digger"),
		m_server(server)
	{
	}

	~Module()
	{}

	void init()
	{
		m_server->sub_event(this, Event::t("core:start"));
		m_server->sub_event(this, Event::t("core:continue"));
		m_server->sub_event(this, Event::t("core:tick"));
		m_server->sub_event(this, Event::t("client_file:files_transmitted"));
	}

	void event(const Event::Type &type, const Event::Private *p)
	{
		EVENT_VOIDN("core:start", on_start)
		EVENT_VOIDN("core:continue", on_continue)
		EVENT_TYPEN("core:tick", on_tick, interface::TickEvent)
		EVENT_TYPEN("client_file:files_transmitted", on_files_transmitted,
				client_file::FilesTransmitted)
	}

	void on_start()
	{
	}

	void on_continue()
	{
	}

	void update_scene()
	{
	}

	void on_tick(const interface::TickEvent &event)
	{
	}

	void on_files_transmitted(const client_file::FilesTransmitted &event)
	{
		network::access(m_server, [&](network::Interface *inetwork){
			inetwork->send(event.recipient, "core:run_script",
					"buildat.run_script_file(\"main/init.lua\")");
		});
	}
};

extern "C" {
	BUILDAT_EXPORT void* createModule_main(interface::Server *server){
		return (void*)(new Module(server));
	}
}
}
// vim: set noet ts=4 sw=4: