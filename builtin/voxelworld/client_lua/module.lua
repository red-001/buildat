-- Buildat: builtin/voxelworld/client_lua/module.lua
-- http://www.apache.org/licenses/LICENSE-2.0
-- Copyright 2014 Perttu Ahola <celeron55@gmail.com>
local log = buildat.Logger("voxelworld")
local magic = require("buildat/extension/urho3d")
local replicate = require("buildat/extension/replicate")
local cereal = require("buildat/extension/cereal")
local dump = buildat.dump
local M = {}

local camera_node = nil
local update_counter = -1
local camera_last_dir = magic.Vector3(0, 0, 0)
local camera_last_p = magic.Vector3(0, 0, 0)

M.chunk_size_voxels = nil
M.section_size_chunks = nil
M.section_size_voxels = nil

function M.init()
	log:info("voxelworld.init()")

	buildat.sub_packet("voxelworld:init", function(data)
		local values = cereal.binary_input(data, {"object",
			{"chunk_size_voxels", {"object",
				{"x", "int32_t"},
				{"y", "int32_t"},
				{"z", "int32_t"},
			}},
			{"section_size_chunks", {"object",
				{"x", "int32_t"},
				{"y", "int32_t"},
				{"z", "int32_t"},
			}},
		})
		log:info(dump(values))
		M.chunk_size_voxels = buildat.IntVector3(values.chunk_size_voxels)
		M.section_size_chunks = buildat.IntVector3(values.section_size_chunks)
		M.section_size_voxels =
				M.chunk_size_voxels:mul_components(M.section_size_chunks)
	end)

	local function setup_buildat_voxel_data(node)
		local data = node:GetVar("buildat_voxel_data"):GetBuffer()
		local registry_name = node:GetVar("buildat_voxel_registry_name"):GetBuffer()
		log:info(dump(node:GetName()).." voxel data size: "..data:GetSize())
		buildat.set_voxel_geometry(node, data, registry_name)
		--node:SetScale(magic.Vector3(1, 1, 1))
	end

	local node_geometry_update_queue = {}

	local function get_next_geometry_update_node()
		-- Find closest one
		local camera_p = camera_node:GetWorldPosition()
		local closest_d = nil
		local closest_k = nil
		for k, node in ipairs(node_geometry_update_queue) do
			local d = (node:GetWorldPosition() - camera_p):Length()
			if closest_d == nil or d < closest_d then
				closest_d = d
				closest_k = k
			end
		end
		return table.remove(node_geometry_update_queue, closest_k)
	end

	magic.SubscribeToEvent("Update", function(event_type, event_data)
		if camera_node and M.section_size_voxels then
			-- TODO: How should this be sent to the server?
			local p = camera_node:GetWorldPosition()
			update_counter = update_counter + 1
			if update_counter % 60 == 0 then
				local section_p = buildat.IntVector3(p):div_components(
						M.section_size_voxels):floor()
				--log:info("p: "..p.x..", "..p.y..", "..p.z.." -> section_p: "..
				--		section_p.x..", "..section_p.y..", "..section_p.z)
				--[[send_get_section(section_p + buildat.IntVector3( 0, 0, 0))
				send_get_section(section_p + buildat.IntVector3(-1, 0, 0))
				send_get_section(section_p + buildat.IntVector3( 1, 0, 0))
				send_get_section(section_p + buildat.IntVector3( 0, 1, 0))
				send_get_section(section_p + buildat.IntVector3( 0,-1, 0))
				send_get_section(section_p + buildat.IntVector3( 0, 0, 1))
				send_get_section(section_p + buildat.IntVector3( 0, 0,-1))]]
			end
		end
		-- Handle geometry updates a few nodes per frame
		if camera_node then
			local camera_dir = camera_node.direction
			local camera_p = camera_node:GetWorldPosition()
			if #node_geometry_update_queue > 0 then
				local nodes_per_frame = 2
				if camera_dir ~= camera_last_dir or camera_p ~= camera_last_p then
					nodes_per_frame = 1 -- Limit when camera is turning
				end
				for i = 1, nodes_per_frame do
					local node = get_next_geometry_update_node()
					if not node then break end
					setup_buildat_voxel_data(node)
				end
			end
			camera_last_dir = camera_dir
			camera_last_p = camera_p
		end
	end)

	replicate.sub_sync_node_added({}, function(node)
		if not node:GetVar("buildat_voxel_data"):IsEmpty() then
			--setup_buildat_voxel_data(node)
			table.insert(node_geometry_update_queue, node)
		end
		local name = node:GetName()
	end)
end

function M.set_camera(new_camera_node)
	camera_node = new_camera_node
end

function send_get_section(p)
	local data = cereal.binary_output({
		p = {
			x = p.x,
			y = p.y,
			z = p.z,
		},
	}, {"object",
		{"p", {"object",
			{"x", "int32_t"},
			{"y", "int32_t"},
			{"z", "int32_t"},
		}},
	})
	--log:info(dump(buildat.bytes(data)))
	buildat.send_packet("voxelworld:get_section", data)
end

return M
-- vim: set noet ts=4 sw=4:
