/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2012  Clifford Wolf <clifford@clifford.at>
 *  
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *  
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "kernel/rtlil.h"
#include "kernel/register.h"
#include "kernel/sigtools.h"
#include "kernel/celltypes.h"
#include "kernel/log.h"
#include <string>
#include <assert.h>

static void print_spice_net(FILE *f, RTLIL::SigSpec s, std::string &neg, std::string &pos, std::string &ncpf, int &nc_counter)
{
	log_assert(s.chunks.size() == 1 && s.chunks[0].width == 1);
	if (s.chunks[0].wire) {
		if (s.chunks[0].wire->width > 1)
			fprintf(f, " %s[%d]", RTLIL::id2cstr(s.chunks[0].wire->name), s.chunks[0].offset);
		else
			fprintf(f, " %s", RTLIL::id2cstr(s.chunks[0].wire->name));
	} else {
		if (s.chunks[0].data.bits.at(0) == RTLIL::State::S0)
			fprintf(f, " %s", neg.c_str());
		else if (s.chunks[0].data.bits.at(0) == RTLIL::State::S1)
			fprintf(f, " %s", pos.c_str());
		else
			fprintf(f, " %s%d", ncpf.c_str(), nc_counter++);
	}
}

static void print_spice_module(FILE *f, RTLIL::Module *module, RTLIL::Design *design, std::string &neg, std::string &pos, std::string &ncpf, bool big_endian)
{
	SigMap sigmap(module);
	int cell_counter = 0, conn_counter = 0, nc_counter = 0;

	for (auto &cell_it : module->cells)
	{
		RTLIL::Cell *cell = cell_it.second;
		fprintf(f, "X%d", cell_counter++);

		std::vector<RTLIL::SigSpec> port_sigs;

		if (design->modules.count(cell->type) == 0)
		{
			log("Warning: no (placeholder) module for cell type `%s' (%s.%s) found! Guessing order of ports.\n",
					RTLIL::id2cstr(cell->type), RTLIL::id2cstr(module->name), RTLIL::id2cstr(cell->name));
			for (auto &conn : cell->connections) {
				RTLIL::SigSpec sig = sigmap(conn.second);
				port_sigs.push_back(sig);
			}
		}
		else
		{
			RTLIL::Module *mod = design->modules.at(cell->type);

			std::vector<RTLIL::Wire*> ports;
			for (auto wire_it : mod->wires) {
				RTLIL::Wire *wire = wire_it.second;
				if (wire->port_id == 0)
					continue;
				while (int(ports.size()) < wire->port_id)
					ports.push_back(NULL);
				ports.at(wire->port_id-1) = wire;
			}

			for (RTLIL::Wire *wire : ports) {
				log_assert(wire != NULL);
				RTLIL::SigSpec sig(RTLIL::State::Sz, wire->width);
				if (cell->connections.count(wire->name) > 0) {
					sig = sigmap(cell->connections.at(wire->name));
					sig.extend(wire->width, false);
				}
				port_sigs.push_back(sig);
			}
		}

		for (auto &sig : port_sigs) {
			for (int i = 0; i < sig.width; i++) {
				RTLIL::SigSpec s = sig.extract(big_endian ? sig.width - 1 - i : i, 1);
				log_assert(s.chunks.size() == 1 && s.chunks[0].width == 1);
				print_spice_net(f, s, neg, pos, ncpf, nc_counter);
			}
		}

		fprintf(f, " %s\n", RTLIL::id2cstr(cell->type));
	}

	for (auto &conn : module->connections)
	for (int i = 0; i < conn.first.width; i++) {
		fprintf(f, "V%d", conn_counter++);
		print_spice_net(f, conn.first.extract(i, 1), neg, pos, ncpf, nc_counter);
		print_spice_net(f, conn.second.extract(i, 1), neg, pos, ncpf, nc_counter);
		fprintf(f, " DC 0\n");
	}
}

struct SpiceBackend : public Backend {
	SpiceBackend() : Backend("spice", "write design to SPICE netlist file") { }
	virtual void help()
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    write_spice [options] [filename]\n");
		log("\n");
		log("Write the current design to an SPICE netlist file.\n");
		log("\n");
		log("    -big_endian\n");
		log("        generate multi-bit ports in MSB first order \n");
		log("        (default is LSB first)\n");
		log("\n");
		log("    -neg net_name\n");
		log("        set the net name for constant 0 (default: Vss)\n");
		log("\n");
		log("    -pos net_name\n");
		log("        set the net name for constant 1 (default: Vdd)\n");
		log("\n");
		log("    -nc_prefix\n");
		log("        prefix for not-connected nets (default: _NC)\n");
		log("\n");
		log("    -top top_module\n");
		log("        set the specified module as design top module\n");
		log("\n");
	}
	virtual void execute(FILE *&f, std::string filename, std::vector<std::string> args, RTLIL::Design *design)
	{
		std::string top_module_name;
		RTLIL::Module *top_module;
		bool big_endian = false;
		std::string neg = "Vss", pos = "Vdd", ncpf = "_NC";

		log_header("Executing SPICE backend.\n");

		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++)
		{
			if (args[argidx] == "-big_endian") {
				big_endian = true;
				continue;
			}
			if (args[argidx] == "-neg" && argidx+1 < args.size()) {
				neg = args[++argidx];
				continue;
			}
			if (args[argidx] == "-pos" && argidx+1 < args.size()) {
				pos = args[++argidx];
				continue;
			}
			if (args[argidx] == "-nc_prefix" && argidx+1 < args.size()) {
				ncpf = args[++argidx];
				continue;
			}
			if (args[argidx] == "-top" && argidx+1 < args.size()) {
				top_module_name = args[++argidx];
				continue;
			}
			break;
		}
		extra_args(f, filename, args, argidx);

		fprintf(f, "* SPICE netlist generated by %s\n", yosys_version_str);
		fprintf(f, "\n");

		for (auto module_it : design->modules)
		{
			RTLIL::Module *module = module_it.second;
			if (module->get_bool_attribute("\\placeholder"))
				continue;

			if (module->processes.size() != 0)
				log_error("Found unmapped processes in module %s: unmapped processes are not supported in SPICE backend!\n", RTLIL::id2cstr(module->name));
			if (module->memories.size() != 0)
				log_error("Found munmapped emories in module %s: unmapped memories are not supported in SPICE backend!\n", RTLIL::id2cstr(module->name));

			if (module->name == RTLIL::escape_id(top_module_name)) {
				top_module = module;
				continue;
			}

			std::vector<RTLIL::Wire*> ports;
			for (auto wire_it : module->wires) {
				RTLIL::Wire *wire = wire_it.second;
				if (wire->port_id == 0)
					continue;
				while (int(ports.size()) < wire->port_id)
					ports.push_back(NULL);
				ports.at(wire->port_id-1) = wire;
			}

			fprintf(f, ".SUBCKT %s", RTLIL::id2cstr(module->name));
			for (RTLIL::Wire *wire : ports) {
				log_assert(wire != NULL);
				if (wire->width > 1) {
					for (int i = 0; i < wire->width; i++)
						fprintf(f, " %s[%d]", RTLIL::id2cstr(wire->name), big_endian ? wire->width - 1 - i : i);
				} else
					fprintf(f, " %s", RTLIL::id2cstr(wire->name));
			}
			fprintf(f, "\n");
			print_spice_module(f, module, design, neg, pos, ncpf, big_endian);
			fprintf(f, ".ENDS %s\n\n", RTLIL::id2cstr(module->name));
		}

		if (!top_module_name.empty()) {
			if (top_module == NULL)
				log_error("Can't find top module `%s'!\n", top_module_name.c_str());
			print_spice_module(f, top_module, design, neg, pos, ncpf, big_endian);
			fprintf(f, "\n");
		}

		fprintf(f, "************************\n");
		fprintf(f, "* end of SPICE netlist *\n");
		fprintf(f, "************************\n");
		fprintf(f, "\n");
	}
} SpiceBackend;

