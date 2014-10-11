
#include "plugin.h"
#include <iostream>
#include <cstdlib>

static bool g_initialized = false;
static Lilv::World* g_world = new Lilv::World();

static Lilv::Node g_audio   = g_world->new_uri(LV2_CORE__AudioPort);
static Lilv::Node g_control = g_world->new_uri(LV2_CORE__ControlPort);
static Lilv::Node g_input   = g_world->new_uri(LV2_CORE__InputPort);
static Lilv::Node g_output  = g_world->new_uri(LV2_CORE__OutputPort);

static Lilv::Node g_atom        = g_world->new_uri(LV2_ATOM__AtomPort);
static Lilv::Node g_atom_chunk  = g_world->new_uri(LV2_ATOM__Chunk);
static Lilv::Node g_atom_seq    = g_world->new_uri(LV2_ATOM__Sequence);
static Lilv::Node g_event       = g_world->new_uri(LV2_EVENT__EventPort);

static Lilv::Node g_integer     = g_world->new_uri(LV2_CORE__integer);
static Lilv::Node g_enumeration = g_world->new_uri(LV2_CORE__enumeration);
static Lilv::Node g_scale_point = g_world->new_uri(LV2_CORE__scalePoint);
static Lilv::Node g_toggled     = g_world->new_uri(LV2_CORE__toggled);
static Lilv::Node g_trigger     = g_world->new_uri(LV2_PORT_PROPS__trigger);
static Lilv::Node g_logarithmic = g_world->new_uri(LV2_PORT_PROPS__logarithmic);

static Lilv::Node g_worker_sched = g_world->new_uri(LV2_WORKER__schedule);
static Lilv::Node g_worker_iface = g_world->new_uri(LV2_WORKER__interface);

URIDMap Plugin::urid_map;

Plugin::URIDs Plugin::urids = {
    urid_map.uri_to_id(LV2_ATOM__Int),
    urid_map.uri_to_id(LV2_ATOM__Float),
    urid_map.uri_to_id(LV2_ATOM__Chunk),
    urid_map.uri_to_id(LV2_ATOM__Path),
    urid_map.uri_to_id(LV2_ATOM__Sequence),
    urid_map.uri_to_id(LV2_ATOM__eventTransfer),
    urid_map.uri_to_id(LV2_MIDI__MidiEvent),
    urid_map.uri_to_id(LV2_TIME__Position),
    urid_map.uri_to_id(LV2_TIME__bar),
    urid_map.uri_to_id(LV2_TIME__barBeat),
    urid_map.uri_to_id(LV2_TIME__beatUnit),
    urid_map.uri_to_id(LV2_TIME__beatsPerBar),
    urid_map.uri_to_id(LV2_TIME__beatsPerMinute),
    urid_map.uri_to_id(LV2_TIME__frame),
    urid_map.uri_to_id(LV2_TIME__speed),
    urid_map.uri_to_id(LV2_BUF_SIZE__minBlockLength),
    urid_map.uri_to_id(LV2_BUF_SIZE__maxBlockLength),
    urid_map.uri_to_id(LV2_BUF_SIZE__sequenceSize),
    urid_map.uri_to_id(LV2_PARAMETERS__sampleRate),
};

// Called by the plugin to schedule non-RT work
static LV2_Worker_Status
work_schedule(LV2_Worker_Schedule_Handle handle,
              uint32_t                   size,
              const void*                data)
{
    Plugin* plugin = (Plugin*)handle;

    // Enqueue message for the worker thread
    return plugin->worker->schedule(size, data) ?
        LV2_WORKER_SUCCESS : LV2_WORKER_ERR_UNKNOWN;
}

// Called by the plugin to respond to non-RT work
static LV2_Worker_Status
work_respond(LV2_Worker_Respond_Handle handle,
             uint32_t                  size,
             const void*               data)
{
    Plugin* plugin = (Plugin*)handle;

    // Enqueue response for the worker
    return plugin->worker->respond(size, data) ?
        LV2_WORKER_SUCCESS : LV2_WORKER_ERR_UNKNOWN;
}

PortGroup::PortGroup(Plugin* p, Lilv::Node type, uint32_t sample_count)
{
    uint32_t i_input = 0, i_output = 0;
    plugin = p;

    for (uint32_t i = 0; i < p->num_ports; i++)
    {
        Lilv::Port port = p->plugin->get_port_by_index(i);

        // check the port type
        if (port.is_a(type) || (port.is_a(g_event) && type == g_atom))
        {
            port_data_t *port_data = 0;

            if (port.is_a(g_input))
            {
                port_data = &inputs_by_index[i_input++];
            }
            else if (port.is_a(g_output))
            {
                port_data = &outputs_by_index[i_output++];
            }

            // port values
            port_data->min = p->ranges.min[i];
            port_data->max = p->ranges.max[i];
            port_data->def = p->ranges.def[i];
            port_data->value = port_data->def;

            // name and symbol
            Lilv::Node symbol = port.get_symbol();
            Lilv::Node name = port.get_name();
            port_data->name = name.as_string();
            port_data->symbol = symbol.as_string();

            // properties
            port_data->is_integer     = port.has_property(g_integer)     ? true : false;
            port_data->is_logarithmic = port.has_property(g_logarithmic) ? true : false;
            port_data->is_enumeration = port.has_property(g_enumeration) ? true : false;
            port_data->is_scale_point = port.has_property(g_scale_point) ? true : false;
            port_data->is_toggled     = port.has_property(g_toggled)     ? true : false;
            port_data->is_trigger     = port.has_property(g_trigger)     ? true : false;

            // check whether has scale points and populate the vectors
            port_data->scale_points.count = 0;
            if (port_data->is_scale_point || port_data->is_enumeration)
            {
                LilvScalePoints* sp_coll = port.get_scale_points();
                LILV_FOREACH(scale_points, i_sp, sp_coll)
                {
                    const LilvScalePoint* sp = lilv_scale_points_get(sp_coll, i_sp);
                    const LilvNode* label = lilv_scale_point_get_label(sp);
                    const LilvNode* value = lilv_scale_point_get_value(sp);

                    if (label && (lilv_node_is_float(value) || lilv_node_is_int(value)))
                    {
                        port_data->scale_points.labels.push_back(lilv_node_as_string(label));
                        port_data->scale_points.values.push_back(lilv_node_as_float(value));
                        port_data->scale_points.count++;
                    }
                }
            }

            // data buffer
            if (sample_count > 1)
            {
                port_data->buffer = new float[sample_count];
            }
            else
            {
                port_data->buffer = &(port_data->value);
            }

            // connect the variable with plugin port
            p->instance->connect_port(i, port_data->buffer);

            // map the ports by symbol
            if (port.is_a(g_input))
            {
                inputs_by_symbol[port_data->symbol] = port_data;
            }
            else if (port.is_a(g_output))
            {
                outputs_by_symbol[port_data->symbol] = port_data;
            }

            // check if is atom or event
            port_data->event_buffer = NULL;
            if (port.is_a(g_atom) || port.is_a(g_event))
            {
                port_data->event_buffer =
                    lv2_evbuf_new(EVENT_BUFFER_SIZE,
                                  port.is_a(g_atom) ? LV2_EVBUF_ATOM : LV2_EVBUF_EVENT,
                                  Plugin::urid_map.map[g_atom_chunk.as_string()],
                                  Plugin::urid_map.map[g_atom_seq.as_string()]);

                p->instance->connect_port(i, lv2_evbuf_get_buffer(port_data->event_buffer));
            }
        }
    }
}

PortGroup::~PortGroup()
{
    // TODO
}

void PortGroup::set_value(std::string preset)
{
    if (preset == MINIMUM_PRESET_LABEL)
    {
        for (uint32_t i = 0; i < inputs_by_index.size(); i++)
        {
            inputs_by_index[i].value = this->inputs_by_index[i].min;
        }
    }
    else if (preset == MAXIMUM_PRESET_LABEL)
    {
        for (uint32_t i = 0; i < this->inputs_by_index.size(); i++)
        {
            inputs_by_index[i].value = this->inputs_by_index[i].max;
        }
    }
    else if (preset == DEFAULT_PRESET_LABEL)
    {
        for (uint32_t i = 0; i < this->inputs_by_index.size(); i++)
        {
            inputs_by_index[i].value = this->inputs_by_index[i].def;
        }
    }

    // TODO: LV2 presets
}

void PortGroup::set_value(uint32_t index, float value)
{
    inputs_by_index[index].value = value;
}

float PortGroup::get_value(uint32_t index)
{
    return inputs_by_index[index].value;
}

Plugin::Plugin(std::string uri, uint32_t sample_rate, uint32_t sample_count)
{
    if (!g_initialized)
    {
        g_world->load_all();
        g_initialized = true;
    }

    this->uri = uri;
    this->sample_rate = sample_rate;
    this->sample_count = sample_count;

    Lilv::Plugins plugins_list = g_world->get_all_plugins();
    Lilv::Node plugin_uri = g_world->new_uri(uri.c_str());
    Lilv::Plugin p = plugins_list.get_by_uri(plugin_uri);
    plugin = &p;

    // get plugin ranges
    num_ports = plugin->get_num_ports();
    ranges.min = new float[num_ports];
    ranges.max = new float[num_ports];
    ranges.def = new float[num_ports];
    plugin->get_port_ranges_float(ranges.min, ranges.max, ranges.def);

    // features
    int n_features = 0;
    features = (LV2_Feature**) calloc(FEATURES_COUNT+1, sizeof(LV2_Feature*));

    // urid
    features[n_features++] = &(urid_map.uri_map_feature);
    features[n_features++] = &(urid_map.urid_map_feature);
    features[n_features++] = &(urid_map.urid_unmap_feature);

    // options
    LV2_Options_Option options[] = {
        { LV2_OPTIONS_INSTANCE, 0, urids.parameters_sampleRate,
          sizeof(int32_t), urids.atom_Int, &sample_rate },
        { LV2_OPTIONS_INSTANCE, 0, urids.bufsize_minBlockLength,
          sizeof(int32_t), urids.atom_Int, &sample_count },
        { LV2_OPTIONS_INSTANCE, 0, urids.bufsize_maxBlockLength,
          sizeof(int32_t), urids.atom_Int, &sample_count },
        { LV2_OPTIONS_INSTANCE, 0, urids.bufsize_sequenceSize,
          sizeof(int32_t), urids.atom_Int, &seq_size },
        { LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, NULL }
    };
    options_feature.URI = LV2_OPTIONS__options;
    options_feature.data = options;
    features[n_features++] = &options_feature;

    // worker schedule
    work_schedule_feature.URI = LV2_WORKER__schedule;
    work_schedule_feature.data = NULL;
    worker = NULL;

    if (plugin->has_feature(g_worker_sched))
    {
        LV2_Worker_Schedule* schedule =
            (LV2_Worker_Schedule*) malloc(sizeof(LV2_Worker_Schedule));

        worker                      = new Worker(this, 4096);
        schedule->handle            = this;
        schedule->schedule_work     = work_schedule;
        work_schedule_feature.data  = schedule;
        features[n_features++]      = &work_schedule_feature;
    }

    // create the plugin instance
    instance = Lilv::Instance::create(p, sample_rate, features);

    // worker interface
    work_iface = NULL;
    if (plugin->has_extension_data(g_worker_iface))
    {
        work_iface =
            (const LV2_Worker_Interface*) instance->get_extension_data(LV2_WORKER__interface);
    }

    // create the ports
    audio = new PortGroup(this, g_audio, sample_count);
    control = new PortGroup(this, g_control);
    atom = new PortGroup(this, g_atom);

    instance->activate();
}

Plugin::~Plugin()
{
    if (ranges.min) delete[] ranges.min;
    if (ranges.max) delete[] ranges.max;
    if (ranges.def) delete[] ranges.def;

    if (worker) delete worker;
    if (audio) delete audio;
    if (control) delete control;
    if (atom) delete atom;
}

void Plugin::run(uint32_t sample_count)
{
    // event input ports
    for (uint32_t i = 0; i < atom->inputs_by_index.size(); i++)
    {
        lv2_evbuf_reset(atom->inputs_by_index[i].event_buffer, true);

        // TODO: write input MIDI events to test
    }

    // reset event output ports
    for (uint32_t i = 0; i < atom->outputs_by_index.size(); i++)
    {
        lv2_evbuf_reset(atom->outputs_by_index[i].event_buffer, false);
    }

    // process the plugin
    instance->run(sample_count);

    // notify the plugin the run cycle is finished
    if (work_iface)
    {
        worker->emit_responses();
        if (work_iface->end_run) work_iface->end_run(instance->get_handle());
    }

    // TODO: write output MIDI events to test
}

int Plugin::work(uint32_t size, const void* data)
{
    return work_iface->work(instance->get_handle(), work_respond, this, size, data);
}

int Plugin::work_response(uint32_t size, const void* data)
{
    return work_iface->work_response(instance->get_handle(), size, data);
}
