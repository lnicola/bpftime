/* SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2022, eunomia-bpf org
 * All rights reserved.
 */
#include "bpftime_shm.hpp"
#include "handler/epoll_handler.hpp"
#include "handler/perf_event_handler.hpp"
#include "spdlog/spdlog.h"
#include <bpftime_shm_internal.hpp>
#include <cstdio>
#include <sys/epoll.h>
#include <unistd.h>
#include <variant>
#include <fstream>
#include <json.hpp>
#include "bpftime_shm_json.hpp"

using namespace bpftime;
using json = nlohmann::json;
using namespace std;

static json bpf_map_attr_to_json(const bpf_map_attr &attr)
{
	json j;
	j["map_type"] = attr.type;
	j["key_size"] = attr.key_size;
	j["value_size"] = attr.value_size;
	j["max_entries"] = attr.max_ents;
	j["flags"] = attr.flags;
	j["ifindex"] = attr.ifindex;
	j["btf_vmlinux_value_type_id"] = attr.btf_vmlinux_value_type_id;
	j["btf_id"] = attr.btf_id;
	j["btf_key_type_id"] = attr.btf_key_type_id;
	j["btf_value_type_id"] = attr.btf_value_type_id;
	j["map_extra"] = attr.map_extra;

	j["kernel_bpf_map_id"] = attr.kernel_bpf_map_id;
	return j;
}

static bpf_map_attr json_to_bpf_map_attr(const json &j)
{
	bpf_map_attr attr;
	attr.type = j["map_type"];
	attr.key_size = j["key_size"];
	attr.value_size = j["value_size"];
	attr.max_ents = j["max_entries"];
	attr.flags = j["flags"];
	attr.ifindex = j["ifindex"];
	attr.btf_vmlinux_value_type_id = j["btf_vmlinux_value_type_id"];
	attr.btf_id = j["btf_id"];
	attr.btf_key_type_id = j["btf_key_type_id"];
	attr.btf_value_type_id = j["btf_value_type_id"];
	attr.map_extra = j["map_extra"];

	attr.kernel_bpf_map_id = j["kernel_bpf_map_id"];
	return attr;
}

static json
bpf_perf_event_handler_attr_to_json(const bpf_perf_event_handler &handler)
{
	json j;
	j["type"] = handler.type;
	j["offset"] = handler.offset;
	j["pid"] = handler.pid;
	j["ref_ctr_off"] = handler.ref_ctr_off;
	j["_module_name"] = handler._module_name;
	j["tracepoint_id"] = handler.tracepoint_id;
	return j;
}

extern "C" int bpftime_import_global_shm_from_json(const char *filename)
{
	return bpftime_import_shm_from_json(shm_holder.global_shared_memory,
					    filename);
}

static int import_shm_handler_from_json(bpftime_shm &shm, json value, int fd)
{
	std::string handler_type = value["type"];
	spdlog::info("import handler type {} fd {}", handler_type, fd);
	if (handler_type == "bpf_prog_handler") {
		std::string insns_str = value["attr"]["insns"];
		std::string name = value["name"];
		int type = value["attr"]["type"];
		int cnt = value["attr"]["cnt"];
		std::vector<ebpf_inst> insns;
		insns.resize(cnt);
		int res =
			hex_string_to_buffer(insns_str,
					     (unsigned char *)insns.data(),
					     insns.size() * sizeof(ebpf_inst));
		if (res < 0) {
			spdlog::error("Failed to parse insns in json");
			return -1;
		}
		shm.add_bpf_prog(fd, insns.data(), cnt, name.c_str(), type);
		for (int perf_fd : value["attr"]["attach_fds"]) {
			shm.add_bpf_prog_attach_target(perf_fd, fd);
		}
	} else if (handler_type == "bpf_map_handler") {
		std::string name = value["name"];
		bpf_map_attr attr = json_to_bpf_map_attr(value["attr"]);
		shm.add_bpf_map(fd, name.c_str(), attr);
	} else if (handler_type == "bpf_perf_event_handler") {
		int type = value["attr"]["type"];
		int offset = value["attr"]["offset"];
		int pid = value["attr"]["pid"];
		int ref_ctr_off = value["attr"]["ref_ctr_off"];
		std::string _module_name = value["attr"]["_module_name"];
		int tracepoint_id = value["attr"]["tracepoint_id"];
		switch ((bpf_event_type)type) {
		case bpf_event_type::BPF_TYPE_UPROBE:
			shm.add_uprobe(fd, pid, _module_name.c_str(), offset,
				       false, ref_ctr_off);
			break;
		case bpf_event_type::BPF_TYPE_URETPROBE:
			shm.add_uprobe(fd, pid, _module_name.c_str(), offset,
				       true, ref_ctr_off);
			break;
		case bpf_event_type::PERF_TYPE_TRACEPOINT:
			shm.add_tracepoint(fd, pid, tracepoint_id);
			break;
		default:
			spdlog::error("Unsupported perf event type {}", type);
			return -1;
		}
	} else if (handler_type == "bpf_link_handler") {
		int prog_fd = value["attr"]["prog_fd"];
		int target_fd = value["attr"]["target_fd"];
		shm.add_bpf_link(fd, prog_fd, target_fd);
	} else {
		spdlog::error("Unsupported handler type {}", handler_type);
		return -1;
	}
	return 0;
}

int bpftime::bpftime_import_shm_handler_from_json(bpftime_shm &shm, int fd,
						  const char *json_string)
{
	json j = json::parse(json_string);
	return import_shm_handler_from_json(shm, fd, j);
}

extern "C" int bpftime_import_shm_handler_from_json(int fd,
						    const char *json_string)
{
	return bpftime::bpftime_import_shm_handler_from_json(
		shm_holder.global_shared_memory, fd, json_string);
}

int bpftime::bpftime_import_shm_from_json(bpftime_shm &shm,
					  const char *filename)
{
	ifstream file(filename);
	json j;
	file >> j;
	file.close();
	for (auto &[key, value] : j.items()) {
		int fd = std::stoi(key);
		spdlog::info("import handler fd {} {}", fd, value.dump());
		int res = import_shm_handler_from_json(shm, value, fd);
		if (res < 0) {
			spdlog::error("Failed to import handler from json");
			return -1;
		}
	}
	return 0;
}

extern "C" int bpftime_export_global_shm_to_json(const char *filename)
{
	return bpftime_export_shm_to_json(shm_holder.global_shared_memory,
					  filename);
}

int bpftime::bpftime_export_shm_to_json(const bpftime_shm &shm,
					const char *filename)
{
	std::ofstream file(filename);
	json j;

	const handler_manager *manager = shm.get_manager();
	if (!manager) {
		spdlog::error("No manager found in the shared memory");
		return -1;
	}
	for (std::size_t i = 0; i < manager->size(); i++) {
		// skip uninitialized handlers
		if (!manager->is_allocated(i)) {
			continue;
		}
		auto &handler = manager->get_handler(i);
		// load the bpf prog
		if (std::holds_alternative<bpf_prog_handler>(handler)) {
			auto &prog_handler =
				std::get<bpf_prog_handler>(handler);
			const ebpf_inst *insns = prog_handler.insns.data();
			size_t cnt = prog_handler.insns.size();
			const char *name = prog_handler.name.c_str();
			// record the prog into json, key is the index of the
			// prog
			json attr = { { "type", prog_handler.type },
				      { "insns",
					buffer_to_hex_string(
						(const unsigned char *)insns,
						sizeof(ebpf_inst) * cnt) },
				      { "cnt", cnt } };
			j[std::to_string(i)] =
				json{ { "type", "bpf_prog_handler" },
				      { "attr", attr },
				      { "name", name } };
			// append attach fds to the json
			for (auto &fd : prog_handler.attach_fds) {
				j[std::to_string(i)]["attr"]["attach_fds"]
					.push_back(fd);
			}
			spdlog::info("find prog fd={} name={}", i,
				     prog_handler.name);
		} else if (std::holds_alternative<bpf_map_handler>(handler)) {
			auto &map_handler = std::get<bpf_map_handler>(handler);
			const char *name = map_handler.name.c_str();
			spdlog::info("bpf_map_handler name={} found at {}",
				     name, i);
			bpf_map_attr attr = map_handler.attr;
			j[std::to_string(i)] = { { "type", "bpf_map_handler" },
						 { "name", name },
						 { "attr", bpf_map_attr_to_json(
								   attr) } };
		} else if (std::holds_alternative<bpf_perf_event_handler>(
				   handler)) {
			auto &perf_handler =
				std::get<bpf_perf_event_handler>(handler);
			j[std::to_string(i)] = {
				{ "type", "bpf_perf_event_handler" },
				{ "attr", bpf_perf_event_handler_attr_to_json(
						  perf_handler) }
			};
			spdlog::info("bpf_perf_event_handler found at {}", i);
		} else if (std::holds_alternative<epoll_handler>(handler)) {
			auto &h = std::get<epoll_handler>(handler);
			j[std::to_string(i)] = { { "type", "epoll_handler" } };
			spdlog::info("epoll_handler found at {}", i);
		} else if (std::holds_alternative<bpf_link_handler>(handler)) {
			auto &h = std::get<bpf_link_handler>(handler);
			j[std::to_string(i)] = {
				{ "type", "bpf_link_handler" },
				{ "attr",
				  { { "prog_fd", h.prog_fd },
				    { "target_fd", h.target_fd } } }
			};
			spdlog::info(
				"bpf_link_handler found at {}，link {} -> {}",
				i, h.prog_fd, h.target_fd);
		} else {
			spdlog::error("Unsupported handler type {}",
				      handler.index());
			return -1;
		}
	}
	// write the json to file
	file << j.dump(4);
	file.close();
	return 0;
}
