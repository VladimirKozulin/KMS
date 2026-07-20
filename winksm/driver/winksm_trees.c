#include "winksm.h"

/*
 * Red-black tree implementation for stable and unstable trees.
 * Ported from Linux KSM's rbtree-based two-tree architecture.
 *
 * Stable tree: keyed by content hash, never rebuilt.
 * Unstable tree: keyed by content hash, rebuilt every full scan.
 */

static WINKSM_RB_NODE g_Sentinel = {0};

static void RB_RotateLeft(WINKSM_RB_NODE **Root, WINKSM_RB_NODE *X) {
    WINKSM_RB_NODE *Y = X->Right;
    X->Right = Y->Left;
    if (Y->Left != &g_Sentinel) {
        Y->Left->Parent = X;
    }
    Y->Parent = X->Parent;
    if (X->Parent == NULL) {
        *Root = Y;
    } else if (X == X->Parent->Left) {
        X->Parent->Left = Y;
    } else {
        X->Parent->Right = Y;
    }
    Y->Left = X;
    X->Parent = Y;
}

static void RB_RotateRight(WINKSM_RB_NODE **Root, WINKSM_RB_NODE *X) {
    WINKSM_RB_NODE *Y = X->Left;
    X->Left = Y->Right;
    if (Y->Right != &g_Sentinel) {
        Y->Right->Parent = X;
    }
    Y->Parent = X->Parent;
    if (X->Parent == NULL) {
        *Root = Y;
    } else if (X == X->Parent->Right) {
        X->Parent->Right = Y;
    } else {
        X->Parent->Left = Y;
    }
    Y->Right = X;
    X->Parent = Y;
}

static void RB_InsertFixup(WINKSM_RB_NODE **Root, WINKSM_RB_NODE *X) {
    while (X != *Root && X->Parent->IsRed) {
        if (X->Parent == X->Parent->Parent->Left) {
            WINKSM_RB_NODE *Y = X->Parent->Parent->Right;
            if (Y->IsRed) {
                X->Parent->IsRed = FALSE;
                Y->IsRed = FALSE;
                X->Parent->Parent->IsRed = TRUE;
                X = X->Parent->Parent;
            } else {
                if (X == X->Parent->Right) {
                    X = X->Parent;
                    RB_RotateLeft(Root, X);
                }
                X->Parent->IsRed = FALSE;
                X->Parent->Parent->IsRed = TRUE;
                RB_RotateRight(Root, X->Parent->Parent);
            }
        } else {
            WINKSM_RB_NODE *Y = X->Parent->Parent->Left;
            if (Y->IsRed) {
                X->Parent->IsRed = FALSE;
                Y->IsRed = FALSE;
                X->Parent->Parent->IsRed = TRUE;
                X = X->Parent->Parent;
            } else {
                if (X == X->Parent->Left) {
                    X = X->Parent;
                    RB_RotateRight(Root, X);
                }
                X->Parent->IsRed = FALSE;
                X->Parent->Parent->IsRed = TRUE;
                RB_RotateLeft(Root, X->Parent->Parent);
            }
        }
    }
    (*Root)->IsRed = FALSE;
}

static void RB_Transplant(WINKSM_RB_NODE **Root, WINKSM_RB_NODE *U, WINKSM_RB_NODE *V) {
    if (U->Parent == NULL) {
        *Root = V;
    } else if (U == U->Parent->Left) {
        U->Parent->Left = V;
    } else {
        U->Parent->Right = V;
    }
    V->Parent = U->Parent;
}

static WINKSM_RB_NODE* RB_Minimum(WINKSM_RB_NODE *X) {
    while (X->Left != &g_Sentinel) {
        X = X->Left;
    }
    return X;
}

static void RB_DeleteFixup(WINKSM_RB_NODE **Root, WINKSM_RB_NODE *X) {
    while (X != *Root && !X->IsRed) {
        if (X == X->Parent->Left) {
            WINKSM_RB_NODE *W = X->Parent->Right;
            if (W->IsRed) {
                W->IsRed = FALSE;
                X->Parent->IsRed = TRUE;
                RB_RotateLeft(Root, X->Parent);
                W = X->Parent->Right;
            }
            if (!W->Left->IsRed && !W->Right->IsRed) {
                W->IsRed = TRUE;
                X = X->Parent;
            } else {
                if (!W->Right->IsRed) {
                    W->Left->IsRed = FALSE;
                    W->IsRed = TRUE;
                    RB_RotateRight(Root, W);
                    W = X->Parent->Right;
                }
                W->IsRed = X->Parent->IsRed;
                X->Parent->IsRed = FALSE;
                W->Right->IsRed = FALSE;
                RB_RotateLeft(Root, X->Parent);
                X = *Root;
            }
        } else {
            WINKSM_RB_NODE *W = X->Parent->Left;
            if (W->IsRed) {
                W->IsRed = FALSE;
                X->Parent->IsRed = TRUE;
                RB_RotateRight(Root, X->Parent);
                W = X->Parent->Left;
            }
            if (!W->Right->IsRed && !W->Left->IsRed) {
                W->IsRed = TRUE;
                X = X->Parent;
            } else {
                if (!W->Left->IsRed) {
                    W->Right->IsRed = FALSE;
                    W->IsRed = TRUE;
                    RB_RotateLeft(Root, W);
                    W = X->Parent->Left;
                }
                W->IsRed = X->Parent->IsRed;
                X->Parent->IsRed = FALSE;
                W->Left->IsRed = FALSE;
                RB_RotateRight(Root, X->Parent);
                X = *Root;
            }
        }
    }
    X->IsRed = FALSE;
}

/* Public API */

void WinKSM_Tree_Init(void) {
    g_Sentinel.IsRed = FALSE;
    g_Sentinel.Left = &g_Sentinel;
    g_Sentinel.Right = &g_Sentinel;
    g_Sentinel.Parent = NULL;
}

static void RB_InsertNode(WINKSM_RB_NODE **Root, WINKSM_RB_NODE *Node,
                           int (*Compare)(WINKSM_RB_NODE*, WINKSM_RB_NODE*)) {
    WINKSM_RB_NODE *Y = NULL;
    WINKSM_RB_NODE *X = *Root;

    while (X != &g_Sentinel) {
        Y = X;
        int cmp = Compare(Node, X);
        if (cmp < 0) {
            X = X->Left;
        } else {
            X = X->Right;
        }
    }

    Node->Parent = Y;
    Node->Left = &g_Sentinel;
    Node->Right = &g_Sentinel;
    Node->IsRed = TRUE;

    if (Y == NULL) {
        *Root = Node;
    } else if (Compare(Node, Y) < 0) {
        Y->Left = Node;
    } else {
        Y->Right = Node;
    }

    RB_InsertFixup(Root, Node);
}

static int CompareStable(WINKSM_RB_NODE *A, WINKSM_RB_NODE *B) {
    WINKSM_STABLE_NODE *a = CONTAINING_RECORD(A, WINKSM_STABLE_NODE, TreeNode);
    WINKSM_STABLE_NODE *b = CONTAINING_RECORD(B, WINKSM_STABLE_NODE, TreeNode);
    if (a->ContentHash < b->ContentHash) return -1;
    if (a->ContentHash > b->ContentHash) return 1;
    return 0;
}

static int CompareUnstable(WINKSM_RB_NODE *A, WINKSM_RB_NODE *B) {
    WINKSM_RMAP_ITEM *a = CONTAINING_RECORD(A, WINKSM_RMAP_ITEM, TreeNode);
    WINKSM_RMAP_ITEM *b = CONTAINING_RECORD(B, WINKSM_RMAP_ITEM, TreeNode);
    if (a->Checksum < b->Checksum) return -1;
    if (a->Checksum > b->Checksum) return 1;
    /* Secondary sort by process ID for deterministic ordering */
    if (a->ProcessId < b->ProcessId) return -1;
    if (a->ProcessId > b->ProcessId) return 1;
    return 0;
}

void WinKSM_Tree_InsertStable(WINKSM_RB_NODE **Root, WINKSM_RB_NODE *Node) {
    RB_InsertNode(Root, Node, CompareStable);
}

void WinKSM_Tree_InsertUnstable(WINKSM_RB_NODE **Root, WINKSM_RB_NODE *Node) {
    RB_InsertNode(Root, Node, CompareUnstable);
}

void WinKSM_Tree_RemoveStable(WINKSM_RB_NODE **Root, WINKSM_RB_NODE *Node) {
    WINKSM_RB_NODE *Y = Node;
    WINKSM_RB_NODE *X;
    BOOLEAN YOriginalRed = Y->IsRed;

    if (Node->Left == &g_Sentinel) {
        X = Node->Right;
        RB_Transplant(Root, Node, Node->Right);
    } else if (Node->Right == &g_Sentinel) {
        X = Node->Left;
        RB_Transplant(Root, Node, Node->Left);
    } else {
        Y = RB_Minimum(Node->Right);
        YOriginalRed = Y->IsRed;
        X = Y->Right;
        if (Y->Parent == Node) {
            X->Parent = Y;
        } else {
            RB_Transplant(Root, Y, Y->Right);
            Y->Right = Node->Right;
            Y->Right->Parent = Y;
        }
        RB_Transplant(Root, Node, Y);
        Y->Left = Node->Left;
        Y->Left->Parent = Y;
        Y->IsRed = Node->IsRed;
    }

    if (!YOriginalRed) {
        RB_DeleteFixup(Root, X);
    }
}

void WinKSM_Tree_RemoveUnstable(WINKSM_RB_NODE **Root, WINKSM_RB_NODE *Node) {
    WinKSM_Tree_RemoveStable(Root, Node);
}

WINKSM_STABLE_NODE* WinKSM_Tree_SearchStable(WINKSM_RB_NODE *Root, uint64_t Hash) {
    WINKSM_RB_NODE *X = Root;
    while (X != &g_Sentinel && X != NULL) {
        WINKSM_STABLE_NODE *node = CONTAINING_RECORD(X, WINKSM_STABLE_NODE, TreeNode);
        if (Hash == node->ContentHash) {
            return node;
        } else if (Hash < node->ContentHash) {
            X = X->Left;
        } else {
            X = X->Right;
        }
    }
    return NULL;
}

WINKSM_RMAP_ITEM* WinKSM_Tree_SearchUnstable(WINKSM_RB_NODE *Root, uint64_t Hash) {
    WINKSM_RB_NODE *X = Root;
    while (X != &g_Sentinel && X != NULL) {
        WINKSM_RMAP_ITEM *item = CONTAINING_RECORD(X, WINKSM_RMAP_ITEM, TreeNode);
        if (Hash == item->Checksum) {
            return item;
        } else if (Hash < item->Checksum) {
            X = X->Left;
        } else {
            X = X->Right;
        }
    }
    return NULL;
}
