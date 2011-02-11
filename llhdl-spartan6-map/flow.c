#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gmp.h>

#include <netlist/net.h>
#include <netlist/manager.h>
#include <netlist/edif.h>
#include <netlist/io.h>
#include <netlist/xilprims.h>
#include <netlist/symbol.h>

#include <llhdl/structure.h>
#include <llhdl/exchange.h>
#include <llhdl/tools.h>

#include <tilm/tilm.h>

struct flow_sc {
	struct llhdl_module *module;
	struct llhdl_node *clock;
	struct netlist_iop_manager *netlist_iop;
	struct netlist_manager *netlist;
	struct netlist_sym_store *symbols;
	struct netlist_instance *vcc;
	struct netlist_instance *gnd;
};

static void find_clock(struct flow_sc *sc)
{
	struct llhdl_node *n;

	sc->clock = NULL;
	n = sc->module->head;
	while(n != NULL) {
		assert(n->type == LLHDL_NODE_SIGNAL);
		if((n->p.signal.source != NULL) && (n->p.signal.source->type == LLHDL_NODE_FD)) {
			if(sc->clock == NULL) {
				assert(n->p.signal.source->p.fd.clock->type == LLHDL_NODE_SIGNAL);
				sc->clock = n->p.signal.source->p.fd.clock;
			} else if(sc->clock != n->p.signal.source->p.fd.clock) {
				fprintf(stderr, "FIXME: multi-clock designs are not implemented yet\n");
				exit(EXIT_FAILURE);
			}
		}
		n = n->p.signal.next;
	}
}

static char *iosuffix(const char *base)
{
	int r;
	char *ret;
	r = asprintf(&ret, "%s_IO", base);
	if(r == -1) abort();
	return ret;
}

static void create_signals(struct flow_sc *sc)
{
	struct llhdl_node *n;
	struct netlist_net *net;
	struct netlist_sym *sym;
	int isout;
	int buf_type;
	int buf_port_fabric, buf_port_pin;
	char *ioname;
	struct netlist_instance *iobuf;
	struct netlist_net *ionet;
	struct netlist_primitive *ioprim;
	struct netlist_instance *ioport;
	
	n = sc->module->head;
	while(n != NULL) {
		assert(n->type == LLHDL_NODE_SIGNAL);
		net = netlist_m_create_net(sc->netlist);
		sym = netlist_sym_add(sc->symbols, net->uid, 'N', n->p.signal.name);
		sym->user = net;
		if(n->p.signal.type != LLHDL_SIGNAL_INTERNAL) {
			isout = n->p.signal.type == LLHDL_SIGNAL_PORT_OUT;
			if(n == sc->clock) {
				assert(!isout);
				buf_type = NETLIST_XIL_BUFGP;
				buf_port_fabric = NETLIST_XIL_BUFGP_O;
				buf_port_pin = NETLIST_XIL_BUFGP_I;
			} else if(isout) {
				buf_type = NETLIST_XIL_OBUF;
				buf_port_fabric = NETLIST_XIL_OBUF_I;
				buf_port_pin = NETLIST_XIL_OBUF_O;
			} else {
				buf_type = NETLIST_XIL_IBUF;
				buf_port_fabric = NETLIST_XIL_IBUF_O;
				buf_port_pin = NETLIST_XIL_IBUF_I;
			}

			/* I/O and clock buffer insertion */
			iobuf = netlist_m_instantiate(sc->netlist, &netlist_xilprims[buf_type]);
			ionet = netlist_m_create_net(sc->netlist);
			ioname = iosuffix(n->p.signal.name);
			sym = netlist_sym_add(sc->symbols, ionet->uid, 'N', ioname);
			sym->user = ionet;
			free(ioname);
			netlist_add_branch(ionet, iobuf, !isout, buf_port_fabric);

			/* Create I/O port and connect to buffer */
			ioprim = netlist_create_io_primitive(sc->netlist_iop, 
				isout ? NETLIST_PRIMITIVE_PORT_OUT : NETLIST_PRIMITIVE_PORT_IN,
				n->p.signal.name);
			ioport = netlist_m_instantiate(sc->netlist, ioprim);
			netlist_add_branch(net, ioport, !isout, 0);
			netlist_add_branch(net, iobuf, isout, buf_port_pin);
		}
		n = n->p.signal.next;
	}
}

static struct netlist_net *resolve_signal(struct flow_sc *sc, struct llhdl_node *n)
{
	struct netlist_sym *sym;
	int is_io;
	char *signame;

	assert(n->type == LLHDL_NODE_SIGNAL);
	is_io = n->p.signal.type != LLHDL_SIGNAL_INTERNAL;
	if(is_io)
		signame = iosuffix(n->p.signal.name);
	else
		signame = n->p.signal.name;
	sym = netlist_sym_lookup(sc->symbols, signame, 'N');
	if(is_io)
		free(signame);
	assert(sym != NULL);

	return sym->user;
}

static void *lut_create(int inputs, mpz_t contents, void *user)
{
	struct flow_sc *sc = user;
	int primitive_type;
	char val[17];
	struct netlist_instance *inst;

	assert(inputs > 0);
	assert(inputs < 7);
	primitive_type = 0;
	switch(inputs) {
		case 1:
			primitive_type = NETLIST_XIL_LUT1;
			gmp_sprintf(val, "%Zx", contents);
			break;
		case 2:
			primitive_type = NETLIST_XIL_LUT2;
			gmp_sprintf(val, "%Zx", contents);
			break;
		case 3:
			primitive_type = NETLIST_XIL_LUT3;
			gmp_sprintf(val, "%02Zx", contents);
			break;
		case 4:
			primitive_type = NETLIST_XIL_LUT4;
			gmp_sprintf(val, "%04Zx", contents);
			break;
		case 5:
			primitive_type = NETLIST_XIL_LUT5;
			gmp_sprintf(val, "%08Zx", contents);
			break;
		case 6:
			primitive_type = NETLIST_XIL_LUT6;
			gmp_sprintf(val, "%16Zx", contents);
			break;
	}
	inst = netlist_m_instantiate(sc->netlist, &netlist_xilprims[primitive_type]);
	netlist_set_attribute(inst, "INIT", val);
	return inst;
}

static void lut_connect(void *a, void *b, int n, void *user)
{
	struct flow_sc *sc = user;
	struct netlist_net *net;
	
	net = netlist_m_create_net(sc->netlist);
	netlist_add_branch(net, a, 1, 0);
	netlist_add_branch(net, b, 0, n);
}

static void map_bdd(struct tilm_param *p, struct llhdl_node *n)
{
	struct flow_sc *sc = p->user;
	struct tilm_result *result;
	struct netlist_net *net;
	int i;
	
	/* Run technology-independent LUT mapper */
	result = tilm_map(p, n->p.signal.source);
	
	/* Special case: no logic was generated, this happens when BDD is identity */
	if(result->out_lut == NULL) {
		/* TODO: join nets */
		free(result);
		return;
	}
	
	/* Connect output of the generated logic */
	net = resolve_signal(sc, n);
	netlist_add_branch(net, result->out_lut, 1, 0);
	
	/* Connect inputs of the generated logic */
	for(i=0;i<result->n_input_assoc;i++) {
		net = resolve_signal(sc, result->input_assoc[i].signal);
		netlist_add_branch(net, result->input_assoc[i].lut, 0, result->input_assoc[i].n);
	}
	
	free(result);
}

static struct netlist_instance *get_gnd_vcc(struct flow_sc *sc, int v)
{
	if(v) {
		if(sc->vcc == NULL)
			sc->vcc = netlist_m_instantiate(sc->netlist, &netlist_xilprims[NETLIST_XIL_VCC]);
		return sc->vcc;
	} else {
		if(sc->gnd == NULL)
			sc->gnd = netlist_m_instantiate(sc->netlist, &netlist_xilprims[NETLIST_XIL_GND]);
		return sc->gnd;
	}
}

static void map_logic(struct flow_sc *sc, int lutmapper, void *lutmapper_extra_param)
{
	struct tilm_param tilm_param;
	struct llhdl_node *n;
	int purity;

	memset(&tilm_param, 0, sizeof(tilm_param));
	tilm_param.mapper_id = lutmapper;
	tilm_param.max_inputs = 6;
	tilm_param.extra_mapper_param = lutmapper_extra_param;
	tilm_param.create = lut_create;
	tilm_param.connect = lut_connect;
	tilm_param.user = sc;
	
	n = sc->module->head;
	while(n != NULL) {
		assert(n->type == LLHDL_NODE_SIGNAL);
		purity = llhdl_is_pure(n->p.signal.source);
		switch(purity) {
			case LLHDL_PURE_EMPTY:
				/* nothing to do */
				break;
			case LLHDL_PURE_SIGNAL:
				/* TODO: join nets */
				break;
			case LLHDL_PURE_CONSTANT:
				assert(n->p.signal.source->type == LLHDL_NODE_BOOLEAN);
				netlist_add_branch(resolve_signal(sc, n),
					get_gnd_vcc(sc, n->p.signal.source->p.boolean.value),
					1, 0);
				break;
			case LLHDL_PURE_BDD:
				map_bdd(&tilm_param, n);
				break;
			default:
				assert(0);
				break;
		}
		n = n->p.signal.next;
	}
}

void run_flow(const char *input_lhd, const char *output_edf, const char *output_sym, const char *part, int lutmapper, void *lutmapper_extra_param)
{
	struct flow_sc sc;
	struct edif_param edif_param;

	/* Initialize */
	sc.module = llhdl_parse_file(input_lhd);
	sc.netlist_iop = netlist_create_iop_manager();
	sc.netlist = netlist_m_new();
	sc.symbols = netlist_sym_newstore();
	sc.vcc = NULL;
	sc.gnd = NULL;

	edif_param.flavor = EDIF_FLAVOR_XILINX;
	edif_param.design_name = sc.module->name;
	edif_param.cell_library = "UNISIMS";
	edif_param.part = (char *)part;
	edif_param.manufacturer = "Xilinx";
	
	/* Find clock (we only support single clock designs atm) */
	find_clock(&sc);
	
	/* Create netlist signals from LLHDL signals.
	 * I/O and clock buffers are also inserted there.
	 */
	create_signals(&sc);
	
	/* Map logic */
	map_logic(&sc, lutmapper, lutmapper_extra_param);
	
	/* Write output files */
	netlist_m_edif_file(sc.netlist, output_edf, &edif_param);
	netlist_sym_to_file(sc.symbols, output_sym);
	
	/* Clean up */
	netlist_sym_freestore(sc.symbols);
	netlist_m_free(sc.netlist);
	netlist_free_iop_manager(sc.netlist_iop);
	llhdl_free_module(sc.module);
}
