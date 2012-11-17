#include "rdis.h"

#include "buffer.h"
#include "gui.h"
#include "index.h"
#include "instruction.h"
#include "label.h"
#include "queue.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>

static const struct _object rdis_callback_object = {
    (void     (*) (void *))         rdis_callback_delete, 
    (void *   (*) (void *))         rdis_callback_copy,
    (int      (*) (void *, void *)) rdis_callback_cmp,
    NULL,
};


static const struct _object rdis_object = {
    (void   (*) (void *))         rdis_delete, 
    NULL,
    NULL,
    NULL,
    (json_t * (*) (void *))         rdis_serialize
};


struct _rdis * rdis_create (_loader * loader)
{
    return rdis_create_with_console(loader, NULL, NULL);
}


struct _rdis * rdis_create_with_console (_loader * loader,
                              void (* console_callback) (void *, const char *),
                              void * console_data)
{
    struct _rdis * rdis;

    rdis = (struct _rdis *) malloc(sizeof(struct _rdis));

    rdis->object = &rdis_object;
    rdis->callback_counter = 0;
    rdis->callbacks        = map_create();
    rdis->loader           = loader;

    rdis->console_data     = console_data;
    rdis->console_callback = console_callback;

    rdis->gui = NULL;

    rdis->functions  = loader_functions(loader);
    rdis_console(rdis, "functions loaded");
    rdis->graph      = loader_graph_functions(loader, rdis->functions);
    rdis_console(rdis, "graph loaded");
    rdis->labels     = loader_labels_functions(loader, rdis->functions);
    rdis_console(rdis, "labels loaded");
    rdis->memory_map = loader_memory_map(loader);
    rdis_console(rdis, "memory loaded");

    // this should be the last thing done so startup script accesses a valid
    // rdis
    rdis->rdis_lua      = rdis_lua_create(rdis);

    return rdis;
}


void rdis_delete (struct _rdis * rdis)
{
    object_delete(rdis->callbacks);
    object_delete(rdis->graph);
    object_delete(rdis->labels);
    object_delete(rdis->functions);
    object_delete(rdis->memory_map);
    if (rdis->loader != NULL)
        object_delete(rdis->loader);
    rdis_lua_delete(rdis->rdis_lua);
    free(rdis);
}


json_t * rdis_serialize (struct _rdis * rdis)
{
    json_t * json = json_object();

    json_object_set(json, "ot",            json_integer(SERIALIZE_RDIS));
    json_object_set(json, "graph",         object_serialize(rdis->graph));
    json_object_set(json, "labels",        object_serialize(rdis->labels));
    json_object_set(json, "function_tree", object_serialize(rdis->functions));
    json_object_set(json, "memory_map",    object_serialize(rdis->memory_map));

    return json;
}


struct _rdis * rdis_deserialize (json_t * json)
{
    json_t * graph      = json_object_get(json, "graph");
    json_t * labels     = json_object_get(json, "labels");
    json_t * functions  = json_object_get(json, "functions");
    json_t * memory_map = json_object_get(json, "memory_map");

    if (! json_is_object(graph))
        return NULL;
    if (! json_is_object(labels))
        return NULL;
    if (! json_is_object(functions))
        return NULL;
    if (! json_is_object(memory_map))
        return NULL;

    struct _graph * ggraph = deserialize(graph);
    if (ggraph == NULL)
        return NULL;
    struct _map * llabels = deserialize(labels);
    if (llabels == NULL) {
        object_delete(ggraph);
        return NULL;
    }
    struct _map * ffunctions = deserialize(functions);
    if (ffunctions == NULL) {
        object_delete(ggraph);
        object_delete(llabels);
        return NULL;
    }
    struct _map * mmemory_map = deserialize(memory_map);
    if (mmemory_map == NULL) {
        object_delete(ggraph);
        object_delete(llabels);
        object_delete(mmemory_map);
        return NULL;
    }

    struct _rdis * rdis = (struct _rdis *) malloc(sizeof(struct _rdis));

    rdis->object           = &rdis_object;
    rdis->callback_counter = 0;
    rdis->callbacks        = map_create();
    rdis->gui              = NULL;
    rdis->loader           = NULL;
    rdis->graph            = ggraph;
    rdis->labels           = llabels;
    rdis->functions        = ffunctions;
    rdis->memory_map       = mmemory_map;

    return rdis;
}


struct _map * rdis_g_references (struct _rdis * rdis)
{
    struct _map * references = map_create();

    struct _graph_it * git;
    // for each node
    for (git = graph_iterator(rdis->graph); git != NULL; git = graph_it_next(git)) {
        struct _graph_node * node = graph_it_node(git);
        struct _list_it * lit;

        // for each instruction
        for (lit = list_iterator(node->data); lit != NULL; lit = lit->next) {
            struct _ins * ins = lit->data;
            struct _list_it * rit;

            // for each reference
            for (rit = list_iterator(ins->references); rit != NULL; rit = rit->next) {
                struct _reference * reference = rit->data;
                int delete_reference = 0;

                if (reference->type == REFERENCE_CONSTANT) {
                    uint64_t lower = map_fetch_max_key(rdis->memory_map, reference->address);
                    struct _buffer * buffer = map_fetch(rdis->memory_map, lower);
                    if (buffer == NULL)
                        continue;
                    uint64_t upper = lower + buffer->size;
                    if (    (reference->address < lower)
                         || (reference->address >= upper))
                        continue;
                    reference = object_copy(reference);
                    reference->type = REFERENCE_CONSTANT_ADDRESSABLE;
                    delete_reference = 1;
                }


                struct _list * ref_list = map_fetch(references, reference->address);
                if (ref_list == NULL) {
                    ref_list = list_create();
                    map_insert(references, reference->address, ref_list);
                    object_delete(ref_list);
                    ref_list = map_fetch(references, reference->address);
                }

                list_append(ref_list, reference);

                if (delete_reference)
                    object_delete(reference);
            }
        }
    }

    return references;
}


void rdis_set_console (struct _rdis * rdis,
                       void (* console_callback) (void *, const char *),
                       void * console_data)
{
    rdis->console_callback = console_callback;
    rdis->console_data     = console_data;
}


void rdis_console (struct _rdis * rdis, const char * string)
{
    if (rdis->console_callback == NULL)
        printf("%s\n", string);
    else
        rdis->console_callback(rdis->console_data, string);
}


void rdis_set_gui (struct _rdis * rdis, struct _gui * gui)
{
    rdis->gui = gui;
}


void rdis_clear_gui (struct _rdis * rdis)
{
    if (rdis->gui != NULL)
        gui_close_windows(rdis->gui);
}


int rdis_user_function (struct _rdis * rdis, uint64_t address)
{
    printf("rdis_user_function %llx\n", (unsigned long long) address);

    // get a tree of all functions reachable at this address
    struct _map * functions = loader_function_address(rdis->loader, address);

    // add in this address as a new function as well
    struct _function * function = function_create(address);
    map_insert(functions, function->address, function);
    object_delete(function);

    // for each newly reachable function
    struct _map_it * mit;
    for (mit = map_iterator(functions); mit != NULL; mit = map_it_next(mit)) {
        struct _function * function = map_it_data(mit);
        uint64_t fitaddress = function->address;

        printf("adding user function: %llx\n", (unsigned long long) fitaddress);

        // if we already have this function, skip it
        if (map_fetch(rdis->functions, function->address) != NULL)
            continue;

        printf("a\n");

        // add this function to the rdis->function_tree
        map_insert(rdis->functions, function->address, function);

        // add label
        struct _label * label = loader_label_address(rdis->loader, fitaddress);
        map_insert(rdis->labels, fitaddress, label);
        object_delete(label);

        // if this function is already in our graph, all we need to do is make
        // sure its a separate node and then remove function predecessors
        struct _graph_node * node = graph_fetch_node_max(rdis->graph, fitaddress);
        if (node != NULL) {

            // already a node, remove function predecessors
            if (node->index == address) {
                remove_function_predecessors(rdis->graph, rdis->functions);
                continue;
            }

            // search for instruction with given address
            struct _list    * ins_list = node->data;
            struct _list_it * it;
            for (it = list_iterator(ins_list); it != NULL; it = it->next) {
                struct _ins * ins = it->data;
                // found instruction
                if (ins->address == fitaddress) {
                    // create a new instruction list.
                    // add remaining instructions to new list while removing them from
                    // the current list
                    struct _list * new_ins_list = list_create();
                    while (1) {
                        list_append(new_ins_list, it->data);
                        it = list_remove(ins_list, it);
                        if (it == NULL)
                            break;
                    }
                    // create a new graph node for this new function
                    graph_add_node(rdis->graph, fitaddress, new_ins_list);
                    // all graph successors from old node are added to new node
                    struct _queue   * queue      = queue_create();
                    struct _list    * successors = graph_node_successors(node);
                    struct _list_it * sit;
                    for (sit = list_iterator(successors);
                         sit != NULL;
                         sit = sit->next) {
                        struct _graph_edge * edge = sit->data;
                        graph_add_edge(rdis->graph,
                                       fitaddress,
                                       edge->tail,
                                       edge->data);
                        queue_push(queue, edge);
                    }
                    object_delete(successors);

                    // and removed from old node
                    while (queue->size > 0) {
                        struct _graph_edge * edge = queue_peek(queue);
                        graph_remove_edge(rdis->graph, edge->head, edge->tail);
                        queue_pop(queue);
                    }
                    object_delete(queue);

                    // that was easy
                    break;
                }
            }
            if (it != NULL)
                continue;
        }

        // we need to create a new graph for this node
        struct _graph * graph = loader_graph_address(rdis->loader, fitaddress);
        graph_merge(rdis->graph, graph);
        object_delete(graph);
    }

    object_delete(functions);

    rdis_callback(rdis);

    return 0;
}


int rdis_function_reachable (struct _rdis * rdis, uint64_t address)
{
    struct _function * function = map_fetch(rdis->functions, address);
    if (function == NULL)
        return -1;
    function->flags |= FUNCTION_REACHABLE;

    // get this function's call graph
    struct _graph * cg = create_call_graph(rdis->graph, address);
    struct _graph_it * git;
    // for each function in the graph
    for (git = graph_iterator(cg); git != NULL; git = graph_it_next(git)) {
        struct _list * ins_list = graph_it_data(git);
        struct _list_it * lit;
        // for each call in the function in the graph
        for (lit = list_iterator(ins_list); lit != NULL; lit = lit->next) {
            struct _ins * ins = lit->data;
            // insure function has correct flags
            if (    (ins->flags & (INS_FLAG_TARGET_SET | INS_FLAG_CALL))
                 == (INS_FLAG_TARGET_SET | INS_FLAG_CALL)) {

                function = map_fetch(rdis->functions, ins->target);
                if (function == NULL)
                    continue;
                function->flags |= FUNCTION_REACHABLE;
            }
        }
    }

    return 0;
}


uint64_t rdis_add_callback (struct _rdis * rdis,
                            void (* callback) (void *),
                            void * data)
{
    struct _rdis_callback * rc = rdis_callback_create(callback, data);
    uint64_t identifier = rdis->callback_counter++;

    rc->identifier = identifier;

    printf("adding callback %p %p %llx\n",
           rc->callback, rc->data, (unsigned long long) rc->identifier);

    map_insert(rdis->callbacks, identifier, rc);
    object_delete(rc);

    return identifier;
}


void rdis_remove_callback (struct _rdis * rdis, uint64_t identifier)
{
    map_remove(rdis->callbacks, identifier);
}


void rdis_callback (struct _rdis * rdis)
{
    struct _map_it * it;

    for (it = map_iterator(rdis->callbacks); it != NULL; it = map_it_next(it)) {
        struct _rdis_callback * rc = map_it_data(it);
        printf("callback %p %llx\n",
               rc->callback, (unsigned long long) rc->identifier);
        fflush(stdout);

        rc->callback(rc->data);
    }
}





struct _rdis_callback * rdis_callback_create (void (* callback) (void *),
                                              void * data)
{
    struct _rdis_callback * rc;

    rc = (struct _rdis_callback *) malloc(sizeof(struct _rdis_callback));
    rc->object     = &rdis_callback_object;
    rc->identifier = 0;
    rc->callback   = callback;
    rc->data       = data;

    return rc;
}


void rdis_callback_delete (struct _rdis_callback * rc)
{
    free(rc);
}


struct _rdis_callback * rdis_callback_copy (struct _rdis_callback * rc)
{
    struct _rdis_callback * rc_copy;

    rc_copy = rdis_callback_create(rc->callback, rc->data);
    rc_copy->identifier = rc->identifier;

    return rc_copy;
}


int rdis_callback_cmp (struct _rdis_callback * lhs, struct _rdis_callback * rhs)
{
    if (lhs->identifier < rhs->identifier)
        return -1;
    else if (lhs->identifier > rhs->identifier)
        return 1;
    return 0;
}