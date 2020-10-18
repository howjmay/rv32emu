#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "c_map.h"

struct c_map_internal {
    struct cmap_node *head;

    /* Properties */
    size_t key_size, elem_size, size;

    c_map_iterator_t it_end, it_most, it_least;

    int (*comparator)(void *, void *);
    void (*destructor)(c_map_node_t *);
};

/*
 * Create a node to be attached in the c_map internal tree structure.
 */
static c_map_node_t *c_map_create_node(void *key,
                                       void *value,
                                       size_t ksize,
                                       size_t vsize)
{
    c_map_node_t *node = malloc(sizeof(struct cmap_node));

    // Allocate memory for the keys and values.
    node->key = malloc(ksize);
    node->data = malloc(vsize);

    // Setup the pointers
    node->left = node->right = node->up = NULL;

    // Set the color to black by default
    node->color = C_MAP_RED;

    /*
     * Copy over the key and values
     *
     * If the parameter passed in is NULL, make the element blank instead of
     * a segfault.
     */
    if (!key)
        memset(node->key, 0, ksize);
    else
        memcpy(node->key, key, ksize);

    if (!value)
        memset(node->data, 0, vsize);
    else
        memcpy(node->data, value, vsize);

    return node;
}

static void c_map_free_node(c_map_t obj, c_map_node_t *node)
{
    if (obj->destructor)
        obj->destructor(node);

    free(node->key);
    free(node->data);
    free(node);
}

/*
 * Perform left rotation with "node". The following happens (with respect
 * to "C"):
 *
 *         B                C
 *        / \              / \
 *       A   C     =>     B   D
 *            \          /
 *             D        A
 *
 * Returns the new node pointing in the spot of the original node.
 */
static c_map_node_t *c_map_rotate_left(c_map_t obj, c_map_node_t *node)
{
    c_map_node_t *r = node->right, *rl = r->left, *up = node->up;

    // Adjust
    r->up = up;
    r->left = node;

    node->right = rl;
    node->up = r;

    if (node->right)
        node->right->up = node;

    if (up) {
        if (up->right == node)
            up->right = r;
        else
            up->left = r;
    }

    if (node == obj->head)
        obj->head = r;

    return r;
}

/*
 * Perform a right rotation with "node". The following happens (with respect
 * to "C"):
 *
 *         C                B
 *        / \              / \
 *       B   D     =>     A   C
 *      /                      \
 *     A                        D
 *
 * Return the new node pointing in the spot of the original node.
 */
static c_map_node_t *c_map_rotate_right(c_map_t obj, c_map_node_t *node)
{
    c_map_node_t *l = node->left, *lr = l->right, *up = node->up;

    // Adjust
    l->up = up;
    l->right = node;

    node->left = lr;
    node->up = l;

    if (node->left)
        node->left->up = node;

    if (up) {
        if (up->right == node)
            up->right = l;
        else
            up->left = l;
    }

    if (node == obj->head)
        obj->head = l;

    return l;
}

static void c_map_l_l(c_map_t obj,
                      c_map_node_t *node UNUSED,
                      c_map_node_t *parent UNUSED,
                      c_map_node_t *grandparent,
                      c_map_node_t *uncle UNUSED)
{
    // Rotate to the right according to grandparent
    grandparent = c_map_rotate_right(obj, grandparent);

    // Swap grandparent and uncle's colors
    c_map_color_t c1 = grandparent->color, c2 = grandparent->right->color;

    grandparent->color = c2;
    grandparent->right->color = c1;
}

static void c_map_l_r(c_map_t obj,
                      c_map_node_t *node,
                      c_map_node_t *parent,
                      c_map_node_t *grandparent,
                      c_map_node_t *uncle)
{
    // Rotate to the left according to parent
    parent = c_map_rotate_left(obj, parent);

    // Refigure out the identity
    node = parent->left;
    grandparent = parent->up;
    uncle =
        (grandparent->left == parent) ? grandparent->right : grandparent->left;

    // Apply left-left case
    c_map_l_l(obj, node, parent, grandparent, uncle);
}

static void c_map_r_r(c_map_t obj,
                      c_map_node_t *node UNUSED,
                      c_map_node_t *parent UNUSED,
                      c_map_node_t *grandparent,
                      c_map_node_t *uncle UNUSED)
{
    // Rotate to the left according to grandparent
    grandparent = c_map_rotate_left(obj, grandparent);

    // Swap grandparent and uncle's colors
    c_map_color_t c1 = grandparent->color, c2 = grandparent->left->color;

    grandparent->color = c2;
    grandparent->left->color = c1;
}

static void c_map_r_l(c_map_t obj,
                      c_map_node_t *node,
                      c_map_node_t *parent,
                      c_map_node_t *grandparent,
                      c_map_node_t *uncle)
{
    // Rotate to the right according to parent
    parent = c_map_rotate_right(obj, parent);

    // Refigure out who is who
    node = parent->right;
    grandparent = parent->up;
    uncle =
        (grandparent->left == parent) ? grandparent->right : grandparent->left;

    // Apply right-right case
    c_map_r_r(obj, node, parent, grandparent, uncle);
}

static void c_map_fix_colors(c_map_t obj, c_map_node_t *node)
{
    // If root, set the color to black
    if (node == obj->head) {
        node->color = C_MAP_BLACK;
        return;
    }

    // If node's parent is black or node is root, back out.
    if (node->up->color == C_MAP_BLACK && node->up != obj->head)
        return;

    // Find out who is who
    c_map_node_t *parent = node->up;
    c_map_node_t *grandparent = parent->up;
    c_map_node_t *uncle;

    if (!parent->up)
        return;

    // Find out the uncle
    if (grandparent->left == parent)
        uncle = grandparent->right;
    else
        uncle = grandparent->left;

    if (uncle && uncle->color == C_MAP_RED) {
        // If the uncle is red...
        // Change color of parent and uncle to black
        uncle->color = C_MAP_BLACK;
        parent->color = C_MAP_BLACK;

        // Change color of grandparent to red.
        grandparent->color = C_MAP_RED;

        // Call this on the grandparent
        c_map_fix_colors(obj, grandparent);
    } else if (!uncle || uncle->color == C_MAP_BLACK) {
        // If the uncle is black...
        if (parent == grandparent->left && node == parent->left)
            c_map_l_l(obj, node, parent, grandparent, uncle);
        else if (parent == grandparent->left && node == parent->right)
            c_map_l_r(obj, node, parent, grandparent, uncle);
        else if (parent == grandparent->right && node == parent->left)
            c_map_r_l(obj, node, parent, grandparent, uncle);
        else if (parent == grandparent->right && node == parent->right)
            c_map_r_r(obj, node, parent, grandparent, uncle);
    }
}

/*
 * Fix the red-black tree post-BST deletion. This may involve multiple
 * recolors and/or rotations depending on which node was deleted, what color
 * it was, and where it was in the tree at the time of deletion.
 *
 * These fixes occur up and down the path of the tree, and each rotation is
 * guaranteed constant time. As such, there is a maximum of O(lg n) operations
 * taking place during the fixup procedure.
 */
static void c_map_delete_fixup(c_map_t obj,
                               c_map_node_t *node,
                               c_map_node_t *p,
                               bool y_is_left,
                               c_map_node_t *y UNUSED)
{
    c_map_node_t *w;
    c_map_color_t lc, rc;

    if (!node)
        return;

    while (node != obj->head && node->color == C_MAP_BLACK) {
        // If left child
        if (y_is_left) {
            w = p->right;

            if (w->color == C_MAP_RED) {
                w->color = C_MAP_BLACK;
                p->color = C_MAP_RED;
                p = c_map_rotate_left(obj, p)->left;
                w = p->right;
            }

            lc = !w->left ? C_MAP_BLACK : w->left->color;
            rc = !w->right ? C_MAP_BLACK : w->right->color;

            if (lc == C_MAP_BLACK && rc == C_MAP_BLACK) {
                w->color = C_MAP_RED;
                node = node->up;
                p = node->up;

                if (p)
                    y_is_left = (node == p->left);
            } else {
                if (rc == C_MAP_BLACK) {
                    w->left->color = C_MAP_BLACK;
                    w->color = C_MAP_RED;
                    w = c_map_rotate_right(obj, w);
                    w = p->right;
                }

                w->color = p->color;
                p->color = C_MAP_BLACK;

                if (w->right)
                    w->right->color = C_MAP_BLACK;

                p = c_map_rotate_left(obj, p);
                node = obj->head;
                p = NULL;
            }
        } else {
            /* Same except flipped "left" and "right" */
            w = p->left;

            if (w->color == C_MAP_RED) {
                w->color = C_MAP_BLACK;
                p->color = C_MAP_RED;
                p = c_map_rotate_right(obj, p)->right;
                w = p->left;
            }

            lc = !w->left ? C_MAP_BLACK : w->left->color;
            rc = !w->right ? C_MAP_BLACK : w->right->color;

            if (lc == C_MAP_BLACK && rc == C_MAP_BLACK) {
                w->color = C_MAP_RED;
                node = node->up;
                p = node->up;
                if (p)
                    y_is_left = (node == p->left);
            } else {
                if (lc == C_MAP_BLACK) {
                    w->right->color = C_MAP_BLACK;
                    w->color = C_MAP_RED;
                    w = c_map_rotate_left(obj, w);
                    w = p->left;
                }

                w->color = p->color;
                p->color = C_MAP_BLACK;

                if (w->left)
                    w->left->color = C_MAP_BLACK;

                p = c_map_rotate_right(obj, p);
                node = obj->head;
                p = NULL;
            }
        }
    }

    node->color = C_MAP_BLACK;
}

/*
 * Recursive wrapper for deleting nodes in a graph at an accelerated pace.
 * Skips rotations. Just aggressively goes through all nodes and deletes.
 */
static void c_map_clear_nested(c_map_t obj, c_map_node_t *node)
{
    // Free children
    if (node->left)
        c_map_clear_nested(obj, node->left);
    if (node->right)
        c_map_clear_nested(obj, node->right);

    // Free self
    c_map_free_node(obj, node);
}

/*
 * Recalculate the positions of the "least" and "most" iterators in the
 * tree. This is so iterators know where the beginning and end of the tree
 * resides.
 */
static void c_map_calibrate(c_map_t obj)
{
    if (!obj->head) {
        obj->it_least.node = obj->it_most.node = NULL;
        return;
    }

    // Recompute it_least and it_most
    obj->it_least.node = obj->it_most.node = obj->head;

    while (obj->it_least.node->left)
        obj->it_least.node = obj->it_least.node->left;

    while (obj->it_most.node->right)
        obj->it_most.node = obj->it_most.node->right;
}

/*
 * Sets up a brand new, blank c_map for use. The size of the node elements
 * is determined by what types are thrown in. "s1" is the size of the key
 * elements in bytes, while "s2" is the size of the value elements in
 * bytes.
 *
 * Since this is also a tree data structure, a comparison function is also
 * required to be passed in. A destruct function is optional and must be
 * added in through another function.
 */
c_map_t new_c_map(size_t s1, size_t s2, int (*cmp)(void *, void *))
{
    c_map_t obj = malloc(sizeof(struct c_map_internal));

    // Set all pointers to NULL
    obj->head = NULL;

    // Set up all default properties
    obj->key_size = s1;
    obj->elem_size = s2;
    obj->size = 0;

    // Function pointers
    obj->comparator = cmp;
    obj->destructor = NULL;

    obj->it_end.prev = obj->it_end.node = NULL;
    obj->it_least.prev = obj->it_least.node = NULL;
    obj->it_most.prev = obj->it_most.node = NULL;
    obj->it_most.node = NULL;

    return obj;
}

/*
 * Insert a key/value pair into the c_map. The value can be blank. If so,
 * it is filled with 0's, as defined in "c_map_create_node".
 */
size_t c_map_insert(c_map_t obj, void *key, void *value)
{
    // Copy the key and value into a new node and prepare it to put into tree.
    c_map_node_t *new_node =
        c_map_create_node(key, value, obj->key_size, obj->elem_size);

    obj->size++;

    if (!obj->head) {
        // Just insert the node in as the new head.
        obj->head = new_node;
        obj->head->color = C_MAP_BLACK;

        // Calibrate the tree to properly assign pointers.
        c_map_calibrate(obj);
        return 1;
    }

    // Traverse the tree until we hit the end or find a side that is NULL
    c_map_node_t *cur = obj->head;

    while (1) {
        int res = obj->comparator(new_node->key, cur->key);

        // If the key matches something else, we can't insert
        if (res == 0) {
            c_map_free_node(obj, new_node);
            return 0;
        }

        if (res < 0) {
            if (!cur->left) {
                cur->left = new_node;
                new_node->up = cur;
                c_map_fix_colors(obj, new_node);
                break;
            }
            cur = cur->left;
        } else {
            if (!cur->right) {
                cur->right = new_node;
                new_node->up = cur;
                c_map_fix_colors(obj, new_node);
                break;
            }
            cur = cur->right;
        }
    }

    c_map_calibrate(obj);

    // Insertion complete.
    return 1;
}

static void c_map_prev(c_map_t obj, c_map_iterator_t *it)
{
    if (!it->node) {
        it->prev = NULL;
        return;
    }

    if (it->node == obj->it_least.node) {
        // We have hit the end.
        it->prev = NULL;  // it->node;
        it->node = NULL;
    } else {
        // To the left, and then as far right as possible.
        if (it->node->left) {
            it->node = it->node->left;

            while (it->node->right)
                it->node = it->node->right;
        } else {
            // Keep going up until there is a left child
            it->prev = it->node;
            it->node = it->node->up;

            if (!it->node)
                return;

            while (it->node->up && it->node->left &&
                   (it->node->left == it->prev)) {
                it->prev = it->node;
                it->node = it->node->up;
            }
        }
    }
}


void c_map_find(c_map_t obj, c_map_iterator_t *it, void *key)
{
    // End the search instantly if there's nothing.
    if (!obj->head) {
        it->node = it->prev = NULL;
        return;
    }

    // Basically a repeat of insert
    c_map_node_t *cur = obj->head;

    // binary search
    while (1) {
        int res = obj->comparator(key, cur->key);

        // If the key matches, we hit our target
        if (res == 0)
            break;
        if (res < 0) {
            if (!cur->left) {
                cur = NULL;
                break;
            }
            cur = cur->left;
        } else {
            if (!cur->right) {
                cur = NULL;
                break;
            }
            cur = cur->right;
        }
    }

    if (cur) {
        it->node = cur;

        // Generate a "prev" too
        c_map_iterator_t tmp = *it;
        c_map_prev(obj, &tmp);
        it->prev = tmp.node;
    } else
        it->node = NULL;
}

bool c_map_empty(c_map_t obj)
{
    return (obj->size == 0);
}

/*
 * Return true if at the the rend of the c_map
 */
bool c_map_at_end(c_map_t obj UNUSED, c_map_iterator_t *it)
{
    return (it->node == NULL);
}

/*
 * Remove a node from the c_map. It performs a BST delete, and then reorders
 * the tree so that it remains balanced.
 */
void c_map_erase(c_map_t obj, c_map_iterator_t *it)
{
    c_map_node_t *x, *y;
    c_map_node_t *node = it->node, *target, *double_blk, *x_parent;

    // If it is the head, and the size is 1, just delete it.
    if (obj->size == 1 && node == obj->head) {
        c_map_free_node(obj, node);
        obj->head = NULL;
        obj->size--;
        return;
    }

    // Determine what the target is
    uint8_t c = 0;
    c |= (node->left != NULL) << 0x0;
    c |= (node->right != NULL) << 0x1;

    // Determine the case
    switch (c) {
    case 0x0:
        // Leaf node (this should be impossible)
        target = node;
        break;

    case 0x1:
        // Has left child
        target = node->left;
        break;

    case 0x2:
        // Has right child
        target = node->right;
        break;

    case 0x3:
        // Has 2 children
        target = node->left;
        while (target->right)
            target = target->right;
        break;
    }
    assert(target);

    // Initially there is no Double Black
    double_blk = NULL;

    if (!node->left || !node->right)
        y = node;
    else
        y = target;

    if (y->left)
        x = y->left;
    else
        x = y->right;

    if (x)
        x->up = y->up;

    x_parent = y->up;

    bool y_is_left = false;
    if (!y->up) {
        obj->head = x;
    } else {
        if (y == y->up->left) {
            y->up->left = x;
            y_is_left = true;
        } else
            y->up->right = x;
    }

    if (y != node) {
        // Move pointers over
        if (obj->destructor)
            obj->destructor(node);

        free(node->key);
        free(node->data);

        node->key = y->key;
        node->data = y->data;

        y->key = y->data = NULL;
    }

    if (y->color == C_MAP_BLACK) {
        // Make a blank node if null
        if (!x) {
            double_blk =
                c_map_create_node(NULL, NULL, obj->key_size, obj->elem_size);

            x = double_blk;

            if (!target->up->left)
                target->up->left = x;
            else
                target->up->right = x;

            x->up = target->up;
            x->color = C_MAP_BLACK;
        }

        // fix the tree up
        c_map_delete_fixup(obj, x, x_parent, y_is_left, y);

        // Clean up Double Black
        if (double_blk) {
            if (double_blk->up) {
                if (double_blk->up->left == double_blk)
                    double_blk->up->left = NULL;
                else
                    double_blk->up->right = NULL;
            }

            c_map_free_node(obj, double_blk);
        }
    }

    obj->size--;

    c_map_free_node(obj, y);
    c_map_calibrate(obj);
}

/*
 * Delete all nodes in the graph. This is done by calling erase on the head
 * node until the tree is emptied.
 */
void c_map_clear(c_map_t obj)
{
    // Aggressively delete by recursion.
    if (obj->head)
        c_map_clear_nested(obj, obj->head);

    // Reset stats
    obj->size = 0;
    obj->head = NULL;
}

/*
 * Free the c_map from memory. Deletes all nodes. Call this when you are done
 * using the data structure.
 */
void c_map_free(c_map_t obj)
{
    // Free all nodes
    c_map_clear(obj);

    // Free the map itself
    free(obj);
}
