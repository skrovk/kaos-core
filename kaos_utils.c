#include "kaos_types.h"
#include "kaos_utils.h"

uint8_t *TAG = "utils";

//  TODO: Create searchable queue structure (hashtable?) 


int tree_left(int index) {
    return 2*index;
}

int tree_right(int index) {
    return 2*index + 1;
}

int parent(int index) {
    return index/2;
}

void local_tree_insert(local_queue_t *local_queue, local_queue_t *new_node) {
    local_queue_t *y;
    local_queue_t *x = local_queue;

    while (x) {
        y = x;
        if (new_node->flow < x->flow) {
            x = x->left;
            continue;
        } else if (new_node->flow > x->flow) {
            x = x->right;
            continue;
        } 

        if (new_node->address < new_node->address) {
            x = x->left;
        } 

        x = x->right;
    }

    new_node->parent = y;
    
    if (!y) {
        local_queue = new_node;
        return;
    }

    if (new_node->flow < y->flow) {
        y->left = new_node;
        return;
    }

    if (new_node->flow > y->flow) {
        y->right = new_node;
        return;
    }

    if (new_node->address < y->address) {
        y->left = new_node;
        return;
    }

    y->right = new_node;
    return;
}


void local_balanced_insert(local_queue_t *local_queue, local_queue_t *new_node) {
    local_tree_insert(local_queue, new_node);

    new_node->colour = 1;
    local_queue_t *y = NULL;

    while ( (new_node != local_queue) && (new_node->parent->colour == 1) ) {
       if ( new_node->parent == new_node->parent->parent->left ) {
           /* If x's parent is a left, y is x's right 'uncle' */
           y = new_node->parent->parent->right;
           if ( y->colour == 1 ) {
               /* case 1 - change the colours */
               new_node->parent->colour = 0;
               y->colour = 0;
               new_node->parent->parent->colour = 1;
               /* Move x up the tree */
               new_node = new_node->parent->parent;
               }
           else {
               /* y is a black node */
               if ( new_node == new_node->parent->right ) {
                   /* and x is to the right */ 
                   /* case 2 - move x up and rotate */
                   new_node = new_node->parent;
                   left_rotate( local_queue, new_node);
                   }
               /* case 3 */
               new_node->parent->colour = 0;
               new_node->parent->parent->colour = 1;
               right_rotate( local_queue, new_node->parent->parent);
            }
        } else {
           /* repeat the "if" part with right and left
              exchanged */
       }
    /* Colour the root black */
    local_queue->colour = 0;
    }
}

// TODO: Find desired queue array msize
int init_local_queue_tree(uint16_t tree_size, local_queue_t **local_queue_tree, ) {
    *local_queue_tree = calloc(tree_size, sizeof(local_queue_t));
}