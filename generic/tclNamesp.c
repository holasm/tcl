/*
 * tclNamesp.c --
 *
 *      Contains support for namespaces, which provide a separate context of
 *      commands and global variables. The global :: namespace is the
 *      traditional Tcl "global" scope. Other namespaces are created as
 *      children of the global namespace. These other namespaces contain
 *      special-purpose commands and variables for packages.  Also includes
 *	the TIP#112 ensemble machinery.
 *
 * Copyright (c) 1993-1997 Lucent Technologies.
 * Copyright (c) 1997 Sun Microsystems, Inc.
 * Copyright (c) 1998-1999 by Scriptics Corporation.
 * Copyright (c) 2002-2003 Donal K. Fellows.
 *
 * Originally implemented by
 *   Michael J. McLennan
 *   Bell Labs Innovations for Lucent Technologies
 *   mmclennan@lucent.com
 *
 * See the file "license.terms" for information on usage and redistribution
 * of this file, and for a DISCLAIMER OF ALL WARRANTIES.
 *
 * RCS: @(#) $Id: tclNamesp.c,v 1.35 2003/12/24 04:18:20 davygrvy Exp $
 */

#include "tclInt.h"

/*
 * Flag passed to TclGetNamespaceForQualName to indicate that it should
 * search for a namespace rather than a command or variable inside a
 * namespace. Note that this flag's value must not conflict with the values
 * of TCL_GLOBAL_ONLY, TCL_NAMESPACE_ONLY, or CREATE_NS_IF_UNKNOWN.
 */

#define FIND_ONLY_NS	0x1000

/*
 * Initial size of stack allocated space for tail list - used when resetting
 * shadowed command references in the functin: TclResetShadowedCmdRefs.
 */

#define NUM_TRAIL_ELEMS 5

/*
 * Count of the number of namespaces created. This value is used as a
 * unique id for each namespace.
 */

static long numNsCreated = 0; 
TCL_DECLARE_MUTEX(nsMutex)

/*
 * This structure contains a cached pointer to a namespace that is the
 * result of resolving the namespace's name in some other namespace. It is
 * the internal representation for a nsName object. It contains the
 * pointer along with some information that is used to check the cached
 * pointer's validity.
 */

typedef struct ResolvedNsName {
    Namespace *nsPtr;		/* A cached namespace pointer. */
    long nsId;			/* nsPtr's unique namespace id. Used to
				 * verify that nsPtr is still valid
				 * (e.g., it's possible that the namespace
				 * was deleted and a new one created at
				 * the same address). */
    Namespace *refNsPtr;	/* Points to the namespace containing the
				 * reference (not the namespace that
				 * contains the referenced namespace). */
    int refCount;		/* Reference count: 1 for each nsName
				 * object that has a pointer to this
				 * ResolvedNsName structure as its internal
				 * rep. This structure can be freed when
				 * refCount becomes zero. */
} ResolvedNsName;

/*
 * The client data for an ensemble command.  This consists of the
 * table of commands that are actually exported by the namespace, and
 * an epoch counter that, combined with the exportLookupEpoch field of
 * the namespace structure, defines whether the table contains valid
 * data or will need to be recomputed next time the ensemble command
 * is called.
 */

typedef struct EnsembleConfig {
    Namespace *nsPtr;		/* The namspace backing this ensemble up. */
    Tcl_Command token;		/* The token for the command that provides
				 * ensemble support for the namespace, or
				 * NULL if the command has been deleted (or
				 * never existed; the global namespace never
				 * has an ensemble command.) */
    int epoch;			/* The epoch at which this ensemble's table of
				 * exported commands is valid. */
    char **subcommandArrayPtr;	/* Array of ensemble subcommand names.  At all
				 * consistent points, this will have the same
				 * number of entries as there are entries in
				 * the subcommandTable hash. */
    Tcl_HashTable subcommandTable;
				/* Hash table of ensemble subcommand names,
				 * which are its keys so this also provides
				 * the storage management for those subcommand
				 * names.  The contents of the entry values are
				 * object version the prefix lists to use when
				 * substituting for the command/subcommand to
				 * build the ensemble implementation command.
				 * Has to be stored here as well as in
				 * subcommandDict because that field is NULL
				 * when we are deriving the ensemble from the
				 * namespace exports list.
				 * FUTURE WORK: use object hash table here. */
    struct EnsembleConfig *next;/* The next ensemble in the linked list of
				 * ensembles associated with a namespace. If
				 * this field points to this ensemble, the
				 * structure has already been unlinked from
				 * all lists, and cannot be found by scanning
				 * the list from the namespace's ensemble
				 * field. */
    int flags;			/* ORed combo of ENS_DEAD and ENS_PREFIX. */

    /* OBJECT FIELDS FOR ENSEMBLE CONFIGURATION */

    Tcl_Obj *subcommandDict;	/* Dictionary providing mapping from
				 * subcommands to their implementing command
				 * prefixes, or NULL if we are to build the
				 * map automatically from the namespace
				 * exports. */
    Tcl_Obj *subcmdList;	/* List of commands that this ensemble
				 * actually provides, and whose implementation
				 * will be built using the subcommandDict (if
				 * present and defined) and by simple mapping
				 * to the namespace otherwise.  If NULL,
				 * indicates that we are using the (dynamic)
				 * list of currently exported commands. */
    Tcl_Obj *unknownHandler;	/* Script prefix used to handle the case when
				 * no match is found (according to the rule
				 * defined by flag bit ENS_PREFIX) or NULL to
				 * use the default error-generating behaviour.
				 * The script execution gets all the arguments
				 * to the ensemble command (including objv[0])
				 * and will have the results passed directly
				 * back to the caller (including the error
				 * code) unless the code is TCL_CONTINUE in
				 * which case the subcommand will be reparsed
				 * by the ensemble core, presumably because
				 * the ensemble itself has been updated. */
} EnsembleConfig;

#define ENS_DEAD	0x1	/* Flag value to say that the ensemble is dead
				 * and on its way out. */
#define ENS_PREFIX	0x2	/* Flag value to say whether to allow
				 * unambiguous prefixes of commands or to
				 * require exact matches for command names. */

/*
 * The data cached in a subcommand's Tcl_Obj rep.  This structure is
 * not shared between Tcl_Objs referring to the same subcommand, even
 * where one is a duplicate of another.
 */

typedef struct EnsembleCmdRep {
    Namespace *nsPtr;		/* The namespace backing the ensemble which
				 * this is a subcommand of. */
    int epoch;			/* Used to confirm when the data in this
				 * really structure matches up with the
				 * ensemble. */
    char *fullSubcmdName;	/* The full (local) name of the subcommand,
				 * allocated with ckalloc(). */
    Tcl_Obj *realPrefixObj;	/* Object containing the prefix words of the
				 * command that implements this ensemble
				 * subcommand. */
} EnsembleCmdRep;

/*
 * Declarations for procedures local to this file:
 */

static void		DeleteImportedCmd _ANSI_ARGS_((ClientData clientData));
static void		DupNsNameInternalRep _ANSI_ARGS_((Tcl_Obj *objPtr,
			    Tcl_Obj *copyPtr));
static void		FreeNsNameInternalRep _ANSI_ARGS_((Tcl_Obj *objPtr));
static int		GetNamespaceFromObj _ANSI_ARGS_((
			    Tcl_Interp *interp, Tcl_Obj *objPtr,
			    Tcl_Namespace **nsPtrPtr));
static int		InvokeImportedCmd _ANSI_ARGS_((
			    ClientData clientData, Tcl_Interp *interp,
			    int objc, Tcl_Obj *CONST objv[]));
static int		NamespaceChildrenCmd _ANSI_ARGS_((
			    ClientData dummy, Tcl_Interp *interp,
			    int objc, Tcl_Obj *CONST objv[]));
static int		NamespaceCodeCmd _ANSI_ARGS_((
			    ClientData dummy, Tcl_Interp *interp,
			    int objc, Tcl_Obj *CONST objv[]));
static int		NamespaceCurrentCmd _ANSI_ARGS_((
			    ClientData dummy, Tcl_Interp *interp,
			    int objc, Tcl_Obj *CONST objv[]));
static int		NamespaceDeleteCmd _ANSI_ARGS_((
			    ClientData dummy, Tcl_Interp *interp,
			    int objc, Tcl_Obj *CONST objv[]));
static int		NamespaceEnsembleCmd _ANSI_ARGS_((
			    ClientData dummy, Tcl_Interp *interp,
			    int objc, Tcl_Obj *CONST objv[]));
static int		NamespaceEvalCmd _ANSI_ARGS_((
			    ClientData dummy, Tcl_Interp *interp,
			    int objc, Tcl_Obj *CONST objv[]));
static int		NamespaceExistsCmd _ANSI_ARGS_((
			    ClientData dummy, Tcl_Interp *interp,
			    int objc, Tcl_Obj *CONST objv[]));
static int		NamespaceExportCmd _ANSI_ARGS_((
			    ClientData dummy, Tcl_Interp *interp,
			    int objc, Tcl_Obj *CONST objv[]));
static int		NamespaceForgetCmd _ANSI_ARGS_((
			    ClientData dummy, Tcl_Interp *interp,
			    int objc, Tcl_Obj *CONST objv[]));
static void		NamespaceFree _ANSI_ARGS_((Namespace *nsPtr));
static int		NamespaceImportCmd _ANSI_ARGS_((
			    ClientData dummy, Tcl_Interp *interp,
			    int objc, Tcl_Obj *CONST objv[]));
static int		NamespaceInscopeCmd _ANSI_ARGS_((
			    ClientData dummy, Tcl_Interp *interp,
			    int objc, Tcl_Obj *CONST objv[]));
static int		NamespaceOriginCmd _ANSI_ARGS_((
			    ClientData dummy, Tcl_Interp *interp,
			    int objc, Tcl_Obj *CONST objv[]));
static int		NamespaceParentCmd _ANSI_ARGS_((
			    ClientData dummy, Tcl_Interp *interp,
			    int objc, Tcl_Obj *CONST objv[]));
static int		NamespaceQualifiersCmd _ANSI_ARGS_((
			    ClientData dummy, Tcl_Interp *interp,
			    int objc, Tcl_Obj *CONST objv[]));
static int		NamespaceTailCmd _ANSI_ARGS_((
			    ClientData dummy, Tcl_Interp *interp,
			    int objc, Tcl_Obj *CONST objv[]));
static int		NamespaceWhichCmd _ANSI_ARGS_((
			    ClientData dummy, Tcl_Interp *interp,
			    int objc, Tcl_Obj *CONST objv[]));
static int		SetNsNameFromAny _ANSI_ARGS_((
			    Tcl_Interp *interp, Tcl_Obj *objPtr));
static void		UpdateStringOfNsName _ANSI_ARGS_((Tcl_Obj *objPtr));
static int		NsEnsembleImplementationCmd _ANSI_ARGS_((
			    ClientData clientData, Tcl_Interp *interp,
			    int objc, Tcl_Obj *CONST objv[]));
static void		BuildEnsembleConfig _ANSI_ARGS_((
			    EnsembleConfig *ensemblePtr));
static int              NsEnsembleStringOrder _ANSI_ARGS_((CONST VOID *strPtr1,
                            CONST VOID *strPtr2));
static void		DeleteEnsembleConfig _ANSI_ARGS_((
			    ClientData clientData));
static void		MakeCachedEnsembleCommand _ANSI_ARGS_((
			    Tcl_Obj *objPtr, EnsembleConfig *ensemblePtr,
			    CONST char *subcmdName, Tcl_Obj *prefixObjPtr));
static void		FreeEnsembleCmdRep _ANSI_ARGS_((Tcl_Obj *objPtr));
static void		DupEnsembleCmdRep _ANSI_ARGS_((Tcl_Obj *objPtr,
			    Tcl_Obj *copyPtr));
static void		StringOfEnsembleCmdRep _ANSI_ARGS_((Tcl_Obj *objPtr));

/*
 * This structure defines a Tcl object type that contains a
 * namespace reference.  It is used in commands that take the
 * name of a namespace as an argument.  The namespace reference
 * is resolved, and the result in cached in the object.
 */

Tcl_ObjType tclNsNameType = {
    "nsName",			/* the type's name */
    FreeNsNameInternalRep,	/* freeIntRepProc */
    DupNsNameInternalRep,	/* dupIntRepProc */
    UpdateStringOfNsName,	/* updateStringProc */
    SetNsNameFromAny		/* setFromAnyProc */
};

/*
 * This structure defines a Tcl object type that contains a reference
 * to an ensemble subcommand (e.g. the "length" in [string length ab])
 * It is used to cache the mapping between the subcommand itself and
 * the real command that implements it.
 */

Tcl_ObjType tclEnsembleCmdType = {
    "ensembleCommand",		/* the type's name */
    FreeEnsembleCmdRep,		/* freeIntRepProc */
    DupEnsembleCmdRep,		/* dupIntRepProc */
    StringOfEnsembleCmdRep,	/* updateStringProc */
    NULL			/* setFromAnyProc */
};

/*
 *----------------------------------------------------------------------
 *
 * TclInitNamespaceSubsystem --
 *
 *	This procedure is called to initialize all the structures that 
 *	are used by namespaces on a per-process basis.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

void
TclInitNamespaceSubsystem()
{
    /*
     * Does nothing for now.
     */
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_GetCurrentNamespace --
 *
 *	Returns a pointer to an interpreter's currently active namespace.
 *
 * Results:
 *	Returns a pointer to the interpreter's current namespace.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

Tcl_Namespace *
Tcl_GetCurrentNamespace(interp)
    register Tcl_Interp *interp; /* Interpreter whose current namespace is
				  * being queried. */
{
    register Interp *iPtr = (Interp *) interp;
    register Namespace *nsPtr;

    if (iPtr->varFramePtr != NULL) {
        nsPtr = iPtr->varFramePtr->nsPtr;
    } else {
        nsPtr = iPtr->globalNsPtr;
    }
    return (Tcl_Namespace *) nsPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_GetGlobalNamespace --
 *
 *	Returns a pointer to an interpreter's global :: namespace.
 *
 * Results:
 *	Returns a pointer to the specified interpreter's global namespace.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

Tcl_Namespace *
Tcl_GetGlobalNamespace(interp)
    register Tcl_Interp *interp; /* Interpreter whose global namespace 
				  * should be returned. */
{
    register Interp *iPtr = (Interp *) interp;
    
    return (Tcl_Namespace *) iPtr->globalNsPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_PushCallFrame --
 *
 *	Pushes a new call frame onto the interpreter's Tcl call stack.
 *	Called when executing a Tcl procedure or a "namespace eval" or
 *	"namespace inscope" command. 
 *
 * Results:
 *	Returns TCL_OK if successful, or TCL_ERROR (along with an error
 *	message in the interpreter's result object) if something goes wrong.
 *
 * Side effects:
 *	Modifies the interpreter's Tcl call stack.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_PushCallFrame(interp, callFramePtr, namespacePtr, isProcCallFrame)
    Tcl_Interp *interp;		 /* Interpreter in which the new call frame
				  * is to be pushed. */
    Tcl_CallFrame *callFramePtr; /* Points to a call frame structure to
				  * push. Storage for this has already been
				  * allocated by the caller; typically this
				  * is the address of a CallFrame structure
				  * allocated on the caller's C stack.  The
				  * call frame will be initialized by this
				  * procedure. The caller can pop the frame
				  * later with Tcl_PopCallFrame, and it is
				  * responsible for freeing the frame's
				  * storage. */
    Tcl_Namespace *namespacePtr; /* Points to the namespace in which the
				  * frame will execute. If NULL, the
				  * interpreter's current namespace will
				  * be used. */
    int isProcCallFrame;	 /* If nonzero, the frame represents a
				  * called Tcl procedure and may have local
				  * vars. Vars will ordinarily be looked up
				  * in the frame. If new variables are
				  * created, they will be created in the
				  * frame. If 0, the frame is for a
				  * "namespace eval" or "namespace inscope"
				  * command and var references are treated
				  * as references to namespace variables. */
{
    Interp *iPtr = (Interp *) interp;
    register CallFrame *framePtr = (CallFrame *) callFramePtr;
    register Namespace *nsPtr;

    if (namespacePtr == NULL) {
	nsPtr = (Namespace *) Tcl_GetCurrentNamespace(interp);
    } else {
        nsPtr = (Namespace *) namespacePtr;
        if (nsPtr->flags & NS_DEAD) {
	    Tcl_Panic("Trying to push call frame for dead namespace");
	    /*NOTREACHED*/
        }
    }

    nsPtr->activationCount++;
    framePtr->nsPtr = nsPtr;
    framePtr->isProcCallFrame = isProcCallFrame;
    framePtr->objc = 0;
    framePtr->objv = NULL;
    framePtr->callerPtr = iPtr->framePtr;
    framePtr->callerVarPtr = iPtr->varFramePtr;
    if (iPtr->varFramePtr != NULL) {
        framePtr->level = (iPtr->varFramePtr->level + 1);
    } else {
        framePtr->level = 1;
    }
    framePtr->procPtr = NULL; 	   /* no called procedure */
    framePtr->varTablePtr = NULL;  /* and no local variables */
    framePtr->numCompiledLocals = 0;
    framePtr->compiledLocals = NULL;

    /*
     * Push the new call frame onto the interpreter's stack of procedure
     * call frames making it the current frame.
     */

    iPtr->framePtr = framePtr;
    iPtr->varFramePtr = framePtr;
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_PopCallFrame --
 *
 *	Removes a call frame from the Tcl call stack for the interpreter.
 *	Called to remove a frame previously pushed by Tcl_PushCallFrame.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Modifies the call stack of the interpreter. Resets various fields of
 *	the popped call frame. If a namespace has been deleted and
 *	has no more activations on the call stack, the namespace is
 *	destroyed.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_PopCallFrame(interp)
    Tcl_Interp* interp;		/* Interpreter with call frame to pop. */
{
    register Interp *iPtr = (Interp *) interp;
    register CallFrame *framePtr = iPtr->framePtr;
    int saveErrFlag;
    Namespace *nsPtr;

    /*
     * It's important to remove the call frame from the interpreter's stack
     * of call frames before deleting local variables, so that traces
     * invoked by the variable deletion don't see the partially-deleted
     * frame.
     */

    iPtr->framePtr = framePtr->callerPtr;
    iPtr->varFramePtr = framePtr->callerVarPtr;

    /*
     * Delete the local variables. As a hack, we save then restore the
     * ERR_IN_PROGRESS flag in the interpreter. The problem is that there
     * could be unset traces on the variables, which cause scripts to be
     * evaluated. This will clear the ERR_IN_PROGRESS flag, losing stack
     * trace information if the procedure was exiting with an error. The
     * code below preserves the flag. Unfortunately, that isn't really
     * enough: we really should preserve the errorInfo variable too
     * (otherwise a nested error in the trace script will trash errorInfo).
     * What's really needed is a general-purpose mechanism for saving and
     * restoring interpreter state.
     */

    saveErrFlag = (iPtr->flags & ERR_IN_PROGRESS);

    if (framePtr->varTablePtr != NULL) {
        TclDeleteVars(iPtr, framePtr->varTablePtr);
        ckfree((char *) framePtr->varTablePtr);
        framePtr->varTablePtr = NULL;
    }
    if (framePtr->numCompiledLocals > 0) {
        TclDeleteCompiledLocalVars(iPtr, framePtr);
    }

    iPtr->flags |= saveErrFlag;

    /*
     * Decrement the namespace's count of active call frames. If the
     * namespace is "dying" and there are no more active call frames,
     * call Tcl_DeleteNamespace to destroy it.
     */

    nsPtr = framePtr->nsPtr;
    nsPtr->activationCount--;
    if ((nsPtr->flags & NS_DYING)
	    && (nsPtr->activationCount == 0)) {
        Tcl_DeleteNamespace((Tcl_Namespace *) nsPtr);
    }
    framePtr->nsPtr = NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_CreateNamespace --
 *
 *	Creates a new namespace with the given name. If there is no
 *	active namespace (i.e., the interpreter is being initialized),
 *	the global :: namespace is created and returned.
 *
 * Results:
 *	Returns a pointer to the new namespace if successful. If the
 *	namespace already exists or if another error occurs, this routine
 *	returns NULL, along with an error message in the interpreter's
 *	result object.
 *
 * Side effects:
 *	If the name contains "::" qualifiers and a parent namespace does
 *	not already exist, it is automatically created. 
 *
 *----------------------------------------------------------------------
 */

Tcl_Namespace *
Tcl_CreateNamespace(interp, name, clientData, deleteProc)
    Tcl_Interp *interp;             /* Interpreter in which a new namespace
				     * is being created. Also used for
				     * error reporting. */
    CONST char *name;               /* Name for the new namespace. May be a
				     * qualified name with names of ancestor
				     * namespaces separated by "::"s. */
    ClientData clientData;	    /* One-word value to store with
				     * namespace. */
    Tcl_NamespaceDeleteProc *deleteProc;
    				    /* Procedure called to delete client
				     * data when the namespace is deleted.
				     * NULL if no procedure should be
				     * called. */
{
    Interp *iPtr = (Interp *) interp;
    register Namespace *nsPtr, *ancestorPtr;
    Namespace *parentPtr, *dummy1Ptr, *dummy2Ptr;
    Namespace *globalNsPtr = iPtr->globalNsPtr;
    CONST char *simpleName;
    Tcl_HashEntry *entryPtr;
    Tcl_DString buffer1, buffer2;
    int newEntry;

    /*
     * If there is no active namespace, the interpreter is being
     * initialized. 
     */

    if ((globalNsPtr == NULL) && (iPtr->varFramePtr == NULL)) {
	/*
	 * Treat this namespace as the global namespace, and avoid
	 * looking for a parent.
	 */
	
        parentPtr = NULL;
        simpleName = "";
    } else if (*name == '\0') {
	Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
		"can't create namespace \"\": only global namespace can have empty name", (char *) NULL);
	return NULL;
    } else {
	/*
	 * Find the parent for the new namespace.
	 */

	TclGetNamespaceForQualName(interp, name, (Namespace *) NULL,
		/*flags*/ (CREATE_NS_IF_UNKNOWN | TCL_LEAVE_ERR_MSG),
		&parentPtr, &dummy1Ptr, &dummy2Ptr, &simpleName);

	/*
	 * If the unqualified name at the end is empty, there were trailing
	 * "::"s after the namespace's name which we ignore. The new
	 * namespace was already (recursively) created and is pointed to
	 * by parentPtr.
	 */

	if (*simpleName == '\0') {
	    return (Tcl_Namespace *) parentPtr;
	}

        /*
         * Check for a bad namespace name and make sure that the name
	 * does not already exist in the parent namespace.
	 */

        if (Tcl_FindHashEntry(&parentPtr->childTable, simpleName) != NULL) {
	    Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
		    "can't create namespace \"", name,
    	    	    "\": already exists", (char *) NULL);
            return NULL;
        }
    }

    /*
     * Create the new namespace and root it in its parent. Increment the
     * count of namespaces created.
     */


    nsPtr = (Namespace *) ckalloc(sizeof(Namespace));
    nsPtr->name            = (char *) ckalloc((unsigned) (strlen(simpleName)+1));
    strcpy(nsPtr->name, simpleName);
    nsPtr->fullName        = NULL;   /* set below */
    nsPtr->clientData      = clientData;
    nsPtr->deleteProc      = deleteProc;
    nsPtr->parentPtr       = parentPtr;
    Tcl_InitHashTable(&nsPtr->childTable, TCL_STRING_KEYS);
    Tcl_MutexLock(&nsMutex);
    numNsCreated++;
    nsPtr->nsId            = numNsCreated;
    Tcl_MutexUnlock(&nsMutex);
    nsPtr->interp          = interp;
    nsPtr->flags           = 0;
    nsPtr->activationCount = 0;
    nsPtr->refCount        = 0;
    Tcl_InitHashTable(&nsPtr->cmdTable, TCL_STRING_KEYS);
    Tcl_InitHashTable(&nsPtr->varTable, TCL_STRING_KEYS);
    nsPtr->exportArrayPtr  = NULL;
    nsPtr->numExportPatterns = 0;
    nsPtr->maxExportPatterns = 0;
    nsPtr->cmdRefEpoch       = 0;
    nsPtr->resolverEpoch     = 0;
    nsPtr->cmdResProc        = NULL;
    nsPtr->varResProc        = NULL;
    nsPtr->compiledVarResProc = NULL;
    nsPtr->exportLookupEpoch = 0;
    nsPtr->ensembles	     = NULL;

    if (parentPtr != NULL) {
        entryPtr = Tcl_CreateHashEntry(&parentPtr->childTable, simpleName,
	        &newEntry);
        Tcl_SetHashValue(entryPtr, (ClientData) nsPtr);
    }

    /*
     * Build the fully qualified name for this namespace.
     */

    Tcl_DStringInit(&buffer1);
    Tcl_DStringInit(&buffer2);
    for (ancestorPtr = nsPtr;  ancestorPtr != NULL;
	    ancestorPtr = ancestorPtr->parentPtr) {
        if (ancestorPtr != globalNsPtr) {
            Tcl_DStringAppend(&buffer1, "::", 2);
            Tcl_DStringAppend(&buffer1, ancestorPtr->name, -1);
        }
        Tcl_DStringAppend(&buffer1, Tcl_DStringValue(&buffer2), -1);

        Tcl_DStringSetLength(&buffer2, 0);
        Tcl_DStringAppend(&buffer2, Tcl_DStringValue(&buffer1), -1);
        Tcl_DStringSetLength(&buffer1, 0);
    }
    
    name = Tcl_DStringValue(&buffer2);
    nsPtr->fullName = (char *) ckalloc((unsigned) (strlen(name)+1));
    strcpy(nsPtr->fullName, name);

    Tcl_DStringFree(&buffer1);
    Tcl_DStringFree(&buffer2);

    /*
     * Return a pointer to the new namespace.
     */

    return (Tcl_Namespace *) nsPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_DeleteNamespace --
 *
 *	Deletes a namespace and all of the commands, variables, and other
 *	namespaces within it.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	When a namespace is deleted, it is automatically removed as a
 *	child of its parent namespace. Also, all its commands, variables
 *	and child namespaces are deleted.
 *
 *----------------------------------------------------------------------
 */

void
Tcl_DeleteNamespace(namespacePtr)
    Tcl_Namespace *namespacePtr;   /* Points to the namespace to delete. */
{
    register Namespace *nsPtr = (Namespace *) namespacePtr;
    Interp *iPtr = (Interp *) nsPtr->interp;
    Namespace *globalNsPtr =
	    (Namespace *) Tcl_GetGlobalNamespace((Tcl_Interp *) iPtr);
    Tcl_HashEntry *entryPtr;

    /*
     * If the namespace has associated ensemble commands, delete them
     * first.  This leaves the actual contents of the namespace alone
     * (unless they are linked ensemble commands, of course.)  Note
     * that this code is actually reentrant so command delete traces
     * won't purturb things badly.
     */

    while (nsPtr->ensembles != NULL) {
	/*
	 * Splice out and link to indicate that we've already been
	 * killed.
	 */
	EnsembleConfig *ensemblePtr = (EnsembleConfig *) nsPtr->ensembles;
	nsPtr->ensembles = (Tcl_Ensemble *) ensemblePtr->next;
	ensemblePtr->next = ensemblePtr;
	Tcl_DeleteCommandFromToken(nsPtr->interp, ensemblePtr->token);
    }

    /*
     * If the namespace is on the call frame stack, it is marked as "dying"
     * (NS_DYING is OR'd into its flags): the namespace can't be looked up
     * by name but its commands and variables are still usable by those
     * active call frames. When all active call frames referring to the
     * namespace have been popped from the Tcl stack, Tcl_PopCallFrame will
     * call this procedure again to delete everything in the namespace.
     * If no nsName objects refer to the namespace (i.e., if its refCount 
     * is zero), its commands and variables are deleted and the storage for
     * its namespace structure is freed. Otherwise, if its refCount is
     * nonzero, the namespace's commands and variables are deleted but the
     * structure isn't freed. Instead, NS_DEAD is OR'd into the structure's
     * flags to allow the namespace resolution code to recognize that the
     * namespace is "deleted". The structure's storage is freed by
     * FreeNsNameInternalRep when its refCount reaches 0.
     */

    if (nsPtr->activationCount > 0) {
        nsPtr->flags |= NS_DYING;
        if (nsPtr->parentPtr != NULL) {
            entryPtr = Tcl_FindHashEntry(&nsPtr->parentPtr->childTable,
		    nsPtr->name);
            if (entryPtr != NULL) {
                Tcl_DeleteHashEntry(entryPtr);
            }
        }
        nsPtr->parentPtr = NULL;
    } else {
	/*
	 * Delete the namespace and everything in it. If this is the global
	 * namespace, then clear it but don't free its storage unless the
	 * interpreter is being torn down.
	 */

        TclTeardownNamespace(nsPtr);

        if ((nsPtr != globalNsPtr) || (iPtr->flags & DELETED)) {
            /*
	     * If this is the global namespace, then it may have residual
             * "errorInfo" and "errorCode" variables for errors that
             * occurred while it was being torn down.  Try to clear the
             * variable list one last time.
	     */

            TclDeleteVars((Interp *) nsPtr->interp, &nsPtr->varTable);
	    
            Tcl_DeleteHashTable(&nsPtr->childTable);
            Tcl_DeleteHashTable(&nsPtr->cmdTable);

            /*
             * If the reference count is 0, then discard the namespace.
             * Otherwise, mark it as "dead" so that it can't be used.
             */

            if (nsPtr->refCount == 0) {
                NamespaceFree(nsPtr);
            } else {
                nsPtr->flags |= NS_DEAD;
            }
        }
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TclTeardownNamespace --
 *
 *	Used internally to dismantle and unlink a namespace when it is
 *	deleted. Divorces the namespace from its parent, and deletes all
 *	commands, variables, and child namespaces.
 *
 *	This is kept separate from Tcl_DeleteNamespace so that the global
 *	namespace can be handled specially. Global variables like
 *	"errorInfo" and "errorCode" need to remain intact while other
 *	namespaces and commands are torn down, in case any errors occur.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Removes this namespace from its parent's child namespace hashtable.
 *	Deletes all commands, variables and namespaces in this namespace.
 *	If this is the global namespace, the "errorInfo" and "errorCode"
 *	variables are left alone and deleted later.
 *
 *----------------------------------------------------------------------
 */

void
TclTeardownNamespace(nsPtr)
    register Namespace *nsPtr;	/* Points to the namespace to be dismantled
				 * and unlinked from its parent. */
{
    Interp *iPtr = (Interp *) nsPtr->interp;
    register Tcl_HashEntry *entryPtr;
    Tcl_HashSearch search;
    Tcl_Namespace *childNsPtr;
    Tcl_Command cmd;
    Namespace *globalNsPtr =
	    (Namespace *) Tcl_GetGlobalNamespace((Tcl_Interp *) iPtr);
    int i;

    /*
     * Start by destroying the namespace's variable table,
     * since variables might trigger traces.
     */

    if (nsPtr == globalNsPtr) {
	/*
	 * This is the global namespace, so be careful to preserve the
	 * "errorInfo" and "errorCode" variables. These might be needed
	 * later on if errors occur while deleting commands. We are careful
	 * to destroy and recreate the "errorInfo" and "errorCode"
	 * variables, in case they had any traces on them.
	 */
    
        CONST char *str;
        char *errorInfoStr, *errorCodeStr;

        str = Tcl_GetVar((Tcl_Interp *) iPtr, "errorInfo", TCL_GLOBAL_ONLY);
        if (str != NULL) {
            errorInfoStr = ckalloc((unsigned) (strlen(str)+1));
            strcpy(errorInfoStr, str);
        } else {
            errorInfoStr = NULL;
        }

        str = Tcl_GetVar((Tcl_Interp *) iPtr, "errorCode", TCL_GLOBAL_ONLY);
        if (str != NULL) {
            errorCodeStr = ckalloc((unsigned) (strlen(str)+1));
            strcpy(errorCodeStr, str);
        } else {
            errorCodeStr = NULL;
        }

        TclDeleteVars(iPtr, &nsPtr->varTable);
        Tcl_InitHashTable(&nsPtr->varTable, TCL_STRING_KEYS);

        if (errorInfoStr != NULL) {
            Tcl_SetVar((Tcl_Interp *) iPtr, "errorInfo", errorInfoStr,
                TCL_GLOBAL_ONLY);
            ckfree(errorInfoStr);
        }
        if (errorCodeStr != NULL) {
            Tcl_SetVar((Tcl_Interp *) iPtr, "errorCode", errorCodeStr,
                TCL_GLOBAL_ONLY);
            ckfree(errorCodeStr);
        }
    } else {
	/*
	 * Variable table should be cleared but not freed! TclDeleteVars
	 * frees it, so we reinitialize it afterwards.
	 */
    
        TclDeleteVars(iPtr, &nsPtr->varTable);
        Tcl_InitHashTable(&nsPtr->varTable, TCL_STRING_KEYS);
    }

    /*
     * Remove the namespace from its parent's child hashtable.
     */

    if (nsPtr->parentPtr != NULL) {
        entryPtr = Tcl_FindHashEntry(&nsPtr->parentPtr->childTable,
	        nsPtr->name);
        if (entryPtr != NULL) {
            Tcl_DeleteHashEntry(entryPtr);
        }
    }
    nsPtr->parentPtr = NULL;

    /*
     * Delete all the child namespaces.
     *
     * BE CAREFUL: When each child is deleted, it will divorce
     *    itself from its parent. You can't traverse a hash table
     *    properly if its elements are being deleted. We use only
     *    the Tcl_FirstHashEntry function to be safe.
     */

    for (entryPtr = Tcl_FirstHashEntry(&nsPtr->childTable, &search);
            entryPtr != NULL;
            entryPtr = Tcl_FirstHashEntry(&nsPtr->childTable, &search)) {
        childNsPtr = (Tcl_Namespace *) Tcl_GetHashValue(entryPtr);
        Tcl_DeleteNamespace(childNsPtr);
    }

    /*
     * Delete all commands in this namespace. Be careful when traversing the
     * hash table: when each command is deleted, it removes itself from the
     * command table.
     */

    for (entryPtr = Tcl_FirstHashEntry(&nsPtr->cmdTable, &search);
            entryPtr != NULL;
            entryPtr = Tcl_FirstHashEntry(&nsPtr->cmdTable, &search)) {
        cmd = (Tcl_Command) Tcl_GetHashValue(entryPtr);
        Tcl_DeleteCommandFromToken((Tcl_Interp *) iPtr, cmd);
    }
    Tcl_DeleteHashTable(&nsPtr->cmdTable);
    Tcl_InitHashTable(&nsPtr->cmdTable, TCL_STRING_KEYS);

    /*
     * Free the namespace's export pattern array.
     */

    if (nsPtr->exportArrayPtr != NULL) {
	for (i = 0;  i < nsPtr->numExportPatterns;  i++) {
	    ckfree(nsPtr->exportArrayPtr[i]);
	}
        ckfree((char *) nsPtr->exportArrayPtr);
	nsPtr->exportArrayPtr = NULL;
	nsPtr->numExportPatterns = 0;
	nsPtr->maxExportPatterns = 0;
    }

    /*
     * Free any client data associated with the namespace.
     */

    if (nsPtr->deleteProc != NULL) {
        (*nsPtr->deleteProc)(nsPtr->clientData);
    }
    nsPtr->deleteProc = NULL;
    nsPtr->clientData = NULL;

    /*
     * Reset the namespace's id field to ensure that this namespace won't
     * be interpreted as valid by, e.g., the cache validation code for
     * cached command references in Tcl_GetCommandFromObj.
     */

    nsPtr->nsId = 0;
}

/*
 *----------------------------------------------------------------------
 *
 * NamespaceFree --
 *
 *	Called after a namespace has been deleted, when its
 *	reference count reaches 0.  Frees the data structure
 *	representing the namespace.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static void
NamespaceFree(nsPtr)
    register Namespace *nsPtr;	/* Points to the namespace to free. */
{
    /*
     * Most of the namespace's contents are freed when the namespace is
     * deleted by Tcl_DeleteNamespace. All that remains is to free its names
     * (for error messages), and the structure itself.
     */

    ckfree(nsPtr->name);
    ckfree(nsPtr->fullName);

    ckfree((char *) nsPtr);
}


/*
 *----------------------------------------------------------------------
 *
 * Tcl_Export --
 *
 *	Makes all the commands matching a pattern available to later be
 *	imported from the namespace specified by namespacePtr (or the
 *	current namespace if namespacePtr is NULL). The specified pattern is
 *	appended onto the namespace's export pattern list, which is
 *	optionally cleared beforehand.
 *
 * Results:
 *	Returns TCL_OK if successful, or TCL_ERROR (along with an error
 *	message in the interpreter's result) if something goes wrong.
 *
 * Side effects:
 *	Appends the export pattern onto the namespace's export list.
 *	Optionally reset the namespace's export pattern list.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_Export(interp, namespacePtr, pattern, resetListFirst)
    Tcl_Interp *interp;		 /* Current interpreter. */
    Tcl_Namespace *namespacePtr; /* Points to the namespace from which 
				  * commands are to be exported. NULL for
                                  * the current namespace. */
    CONST char *pattern;         /* String pattern indicating which commands
                                  * to export. This pattern may not include
				  * any namespace qualifiers; only commands
				  * in the specified namespace may be
				  * exported. */
    int resetListFirst;		 /* If nonzero, resets the namespace's
				  * export list before appending. */
{
#define INIT_EXPORT_PATTERNS 5    
    Namespace *nsPtr, *exportNsPtr, *dummyPtr;
    Namespace *currNsPtr = (Namespace *) Tcl_GetCurrentNamespace(interp);
    CONST char *simplePattern;
    char *patternCpy;
    int neededElems, len, i;

    /*
     * If the specified namespace is NULL, use the current namespace.
     */

    if (namespacePtr == NULL) {
        nsPtr = (Namespace *) currNsPtr;
    } else {
        nsPtr = (Namespace *) namespacePtr;
    }

    /*
     * If resetListFirst is true (nonzero), clear the namespace's export
     * pattern list.
     */

    if (resetListFirst) {
	if (nsPtr->exportArrayPtr != NULL) {
	    for (i = 0;  i < nsPtr->numExportPatterns;  i++) {
		ckfree(nsPtr->exportArrayPtr[i]);
	    }
	    ckfree((char *) nsPtr->exportArrayPtr);
	    nsPtr->exportArrayPtr = NULL;
	    TclInvalidateNsCmdLookup(nsPtr);
	    nsPtr->numExportPatterns = 0;
	    nsPtr->maxExportPatterns = 0;
	}
    }

    /*
     * Check that the pattern doesn't have namespace qualifiers.
     */

    TclGetNamespaceForQualName(interp, pattern, nsPtr,
	    /*flags*/ TCL_LEAVE_ERR_MSG, &exportNsPtr, &dummyPtr,
	    &dummyPtr, &simplePattern);

    if ((exportNsPtr != nsPtr) || (strcmp(pattern, simplePattern) != 0)) {
	Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
	        "invalid export pattern \"", pattern,
		"\": pattern can't specify a namespace",
		(char *) NULL);
	return TCL_ERROR;
    }

    /*
     * Make sure that we don't already have the pattern in the array
     */
    if (nsPtr->exportArrayPtr != NULL) {
	for (i = 0;  i < nsPtr->numExportPatterns;  i++) {
	    if (strcmp(pattern, nsPtr->exportArrayPtr[i]) == 0) {
		/*
		 * The pattern already exists in the list
		 */
		return TCL_OK;
	    }
	}
    }

    /*
     * Make sure there is room in the namespace's pattern array for the
     * new pattern.
     */

    neededElems = nsPtr->numExportPatterns + 1;
    if (nsPtr->exportArrayPtr == NULL) {
	nsPtr->exportArrayPtr = (char **)
	        ckalloc((unsigned) (INIT_EXPORT_PATTERNS * sizeof(char *)));
	nsPtr->numExportPatterns = 0;
	nsPtr->maxExportPatterns = INIT_EXPORT_PATTERNS;
    } else if (neededElems > nsPtr->maxExportPatterns) {
	int numNewElems = 2 * nsPtr->maxExportPatterns;
	size_t currBytes = nsPtr->numExportPatterns * sizeof(char *);
	size_t newBytes  = numNewElems * sizeof(char *);
	char **newPtr = (char **) ckalloc((unsigned) newBytes);

	memcpy((VOID *) newPtr, (VOID *) nsPtr->exportArrayPtr,
	        currBytes);
	ckfree((char *) nsPtr->exportArrayPtr);
	nsPtr->exportArrayPtr = (char **) newPtr;
	nsPtr->maxExportPatterns = numNewElems;
    }

    /*
     * Add the pattern to the namespace's array of export patterns.
     */

    len = strlen(pattern);
    patternCpy = (char *) ckalloc((unsigned) (len + 1));
    strcpy(patternCpy, pattern);
    
    nsPtr->exportArrayPtr[nsPtr->numExportPatterns] = patternCpy;
    nsPtr->numExportPatterns++;

    /*
     * The list of commands actually exported from the namespace might
     * have changed (probably will have!)  However, we do not need to
     * recompute this just yet; next time we need the info will be
     * soon enough.
     */

    TclInvalidateNsCmdLookup(nsPtr);

    return TCL_OK;
#undef INIT_EXPORT_PATTERNS
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_AppendExportList --
 *
 *	Appends onto the argument object the list of export patterns for the
 *	specified namespace.
 *
 * Results:
 *	The return value is normally TCL_OK; in this case the object
 *	referenced by objPtr has each export pattern appended to it. If an
 *	error occurs, TCL_ERROR is returned and the interpreter's result
 *	holds an error message.
 *
 * Side effects:
 *	If necessary, the object referenced by objPtr is converted into
 *	a list object.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_AppendExportList(interp, namespacePtr, objPtr)
    Tcl_Interp *interp;		 /* Interpreter used for error reporting. */
    Tcl_Namespace *namespacePtr; /* Points to the namespace whose export
				  * pattern list is appended onto objPtr.
				  * NULL for the current namespace. */
    Tcl_Obj *objPtr;		 /* Points to the Tcl object onto which the
				  * export pattern list is appended. */
{
    Namespace *nsPtr;
    int i, result;

    /*
     * If the specified namespace is NULL, use the current namespace.
     */

    if (namespacePtr == NULL) {
        nsPtr = (Namespace *) (Namespace *) Tcl_GetCurrentNamespace(interp);
    } else {
        nsPtr = (Namespace *) namespacePtr;
    }

    /*
     * Append the export pattern list onto objPtr.
     */

    for (i = 0;  i < nsPtr->numExportPatterns;  i++) {
	result = Tcl_ListObjAppendElement(interp, objPtr,
		Tcl_NewStringObj(nsPtr->exportArrayPtr[i], -1));
	if (result != TCL_OK) {
	    return result;
	}
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_Import --
 *
 *	Imports all of the commands matching a pattern into the namespace
 *	specified by namespacePtr (or the current namespace if contextNsPtr
 *	is NULL). This is done by creating a new command (the "imported
 *	command") that points to the real command in its original namespace.
 *
 *      If matching commands are on the autoload path but haven't been
 *	loaded yet, this command forces them to be loaded, then creates
 *	the links to them.
 *
 * Results:
 *	Returns TCL_OK if successful, or TCL_ERROR (along with an error
 *	message in the interpreter's result) if something goes wrong.
 *
 * Side effects:
 *	Creates new commands in the importing namespace. These indirect
 *	calls back to the real command and are deleted if the real commands
 *	are deleted.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_Import(interp, namespacePtr, pattern, allowOverwrite)
    Tcl_Interp *interp;		 /* Current interpreter. */
    Tcl_Namespace *namespacePtr; /* Points to the namespace into which the
				  * commands are to be imported. NULL for
                                  * the current namespace. */
    CONST char *pattern;         /* String pattern indicating which commands
                                  * to import. This pattern should be
				  * qualified by the name of the namespace
				  * from which to import the command(s). */
    int allowOverwrite;		 /* If nonzero, allow existing commands to
				  * be overwritten by imported commands.
				  * If 0, return an error if an imported
				  * cmd conflicts with an existing one. */
{
    Interp *iPtr = (Interp *) interp;
    Namespace *nsPtr, *importNsPtr, *dummyPtr;
    Namespace *currNsPtr = (Namespace *) Tcl_GetCurrentNamespace(interp);
    CONST char *simplePattern;
    char *cmdName;
    register Tcl_HashEntry *hPtr;
    Tcl_HashSearch search;
    Command *cmdPtr, *realCmdPtr;
    ImportRef *refPtr;
    Tcl_Command autoCmd, importedCmd;
    ImportedCmdData *dataPtr;
    int wasExported, i, result;

    /*
     * If the specified namespace is NULL, use the current namespace.
     */

    if (namespacePtr == NULL) {
        nsPtr = (Namespace *) currNsPtr;
    } else {
        nsPtr = (Namespace *) namespacePtr;
    }
 
    /*
     * First, invoke the "auto_import" command with the pattern
     * being imported.  This command is part of the Tcl library.
     * It looks for imported commands in autoloaded libraries and
     * loads them in.  That way, they will be found when we try
     * to create links below.
     */
    
    autoCmd = Tcl_FindCommand(interp, "auto_import",
 	    (Tcl_Namespace *) NULL, /*flags*/ TCL_GLOBAL_ONLY);
 
    if (autoCmd != NULL) {
	Tcl_Obj *objv[2];
 
	objv[0] = Tcl_NewStringObj("auto_import", -1);
	Tcl_IncrRefCount(objv[0]);
	objv[1] = Tcl_NewStringObj(pattern, -1);
	Tcl_IncrRefCount(objv[1]);
 
	cmdPtr = (Command *) autoCmd;
	result = (*cmdPtr->objProc)(cmdPtr->objClientData, interp,
		2, objv);
 
	Tcl_DecrRefCount(objv[0]);
	Tcl_DecrRefCount(objv[1]);
 
	if (result != TCL_OK) {
	    return TCL_ERROR;
	}
	Tcl_ResetResult(interp);
    }

    /*
     * From the pattern, find the namespace from which we are importing
     * and get the simple pattern (no namespace qualifiers or ::'s) at
     * the end.
     */

    if (strlen(pattern) == 0) {
	Tcl_SetStringObj(Tcl_GetObjResult(interp),
	        "empty import pattern", -1);
        return TCL_ERROR;
    }
    TclGetNamespaceForQualName(interp, pattern, nsPtr,
	    /*flags*/ TCL_LEAVE_ERR_MSG, &importNsPtr, &dummyPtr,
	    &dummyPtr, &simplePattern);

    if (importNsPtr == NULL) {
	Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
		"unknown namespace in import pattern \"",
		pattern, "\"", (char *) NULL);
        return TCL_ERROR;
    }
    if (importNsPtr == nsPtr) {
	if (pattern == simplePattern) {
	    Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
		    "no namespace specified in import pattern \"", pattern,
		    "\"", (char *) NULL);
	} else {
	    Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
		    "import pattern \"", pattern,
		    "\" tries to import from namespace \"",
		    importNsPtr->name, "\" into itself", (char *) NULL);
	}
        return TCL_ERROR;
    }

    /*
     * Scan through the command table in the source namespace and look for
     * exported commands that match the string pattern. Create an "imported
     * command" in the current namespace for each imported command; these
     * commands redirect their invocations to the "real" command.
     */

    for (hPtr = Tcl_FirstHashEntry(&importNsPtr->cmdTable, &search);
	    (hPtr != NULL);
	    hPtr = Tcl_NextHashEntry(&search)) {
        cmdName = Tcl_GetHashKey(&importNsPtr->cmdTable, hPtr);
        if (Tcl_StringMatch(cmdName, simplePattern)) {
	    /*
	     * The command cmdName in the source namespace matches the
	     * pattern. Check whether it was exported. If it wasn't,
	     * we ignore it.
	     */

	    wasExported = 0;
	    for (i = 0;  i < importNsPtr->numExportPatterns;  i++) {
		if (Tcl_StringMatch(cmdName,
			importNsPtr->exportArrayPtr[i])) {
		    wasExported = 1;
		    break;
		}
	    }
	    if (!wasExported) {
		continue;
            }

	    /*
	     * Unless there is a name clash, create an imported command
	     * in the current namespace that refers to cmdPtr.
	     */
	    
            if ((Tcl_FindHashEntry(&nsPtr->cmdTable, cmdName) == NULL)
		    || allowOverwrite) {
		/*
		 * Create the imported command and its client data.
		 * To create the new command in the current namespace, 
		 * generate a fully qualified name for it.
		 */

		Tcl_DString ds;

		Tcl_DStringInit(&ds);
		Tcl_DStringAppend(&ds, nsPtr->fullName, -1);
		if (nsPtr != iPtr->globalNsPtr) {
		    Tcl_DStringAppend(&ds, "::", 2);
		}
		Tcl_DStringAppend(&ds, cmdName, -1);

		/*
		 * Check whether creating the new imported command in the
		 * current namespace would create a cycle of imported->real
		 * command references that also would destroy an existing
		 * "real" command already in the current namespace.
		 */

		cmdPtr = (Command *) Tcl_GetHashValue(hPtr);
		if (cmdPtr->deleteProc == DeleteImportedCmd) {
		    realCmdPtr = (Command *) TclGetOriginalCommand(
			    (Tcl_Command) cmdPtr);
		    if ((realCmdPtr != NULL)
			    && (realCmdPtr->nsPtr == currNsPtr)
			    && (Tcl_FindHashEntry(&currNsPtr->cmdTable,
			            cmdName) != NULL)) {
			Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
			        "import pattern \"", pattern,
				"\" would create a loop containing command \"",
				Tcl_DStringValue(&ds), "\"", (char *) NULL);
			Tcl_DStringFree(&ds);
			return TCL_ERROR;
		    }
		}

		dataPtr = (ImportedCmdData *)
		        ckalloc(sizeof(ImportedCmdData));
                importedCmd = Tcl_CreateObjCommand(interp, 
                        Tcl_DStringValue(&ds), InvokeImportedCmd,
                        (ClientData) dataPtr, DeleteImportedCmd);
		dataPtr->realCmdPtr = cmdPtr;
		dataPtr->selfPtr = (Command *) importedCmd;
		dataPtr->selfPtr->compileProc = cmdPtr->compileProc;
		Tcl_DStringFree(&ds);

		/*
		 * Create an ImportRef structure describing this new import
		 * command and add it to the import ref list in the "real"
		 * command.
		 */

                refPtr = (ImportRef *) ckalloc(sizeof(ImportRef));
                refPtr->importedCmdPtr = (Command *) importedCmd;
                refPtr->nextPtr = cmdPtr->importRefPtr;
                cmdPtr->importRefPtr = refPtr;
            } else {
		Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
		        "can't import command \"", cmdName,
			"\": already exists", (char *) NULL);
                return TCL_ERROR;
            }
        }
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_ForgetImport --
 *
 *	Deletes previously imported commands. Given a pattern that may
 *	include the name of an exporting namespace, this procedure first
 *	finds all matching exported commands. It then looks in the namespace
 *	specified by namespacePtr for any corresponding previously imported
 *	commands, which it deletes. If namespacePtr is NULL, commands are
 *	deleted from the current namespace.
 *
 * Results:
 *	Returns TCL_OK if successful. If there is an error, returns
 *	TCL_ERROR and puts an error message in the interpreter's result
 *	object.
 *
 * Side effects:
 *	May delete commands. 
 *
 *----------------------------------------------------------------------
 */

int
Tcl_ForgetImport(interp, namespacePtr, pattern)
    Tcl_Interp *interp;		 /* Current interpreter. */
    Tcl_Namespace *namespacePtr; /* Points to the namespace from which
				  * previously imported commands should be
				  * removed. NULL for current namespace. */
    CONST char *pattern;	 /* String pattern indicating which imported
				  * commands to remove. This pattern should
				  * be qualified by the name of the
				  * namespace from which the command(s) were
				  * imported. */
{
    Namespace *nsPtr, *importNsPtr, *dummyPtr, *actualCtxPtr;
    CONST char *simplePattern;
    char *cmdName;
    register Tcl_HashEntry *hPtr;
    Tcl_HashSearch search;
    Command *cmdPtr;

    /*
     * If the specified namespace is NULL, use the current namespace.
     */

    if (namespacePtr == NULL) {
        nsPtr = (Namespace *) Tcl_GetCurrentNamespace(interp);
    } else {
        nsPtr = (Namespace *) namespacePtr;
    }

    /*
     * From the pattern, find the namespace from which we are importing
     * and get the simple pattern (no namespace qualifiers or ::'s) at
     * the end.
     */

    TclGetNamespaceForQualName(interp, pattern, nsPtr,
	    /*flags*/ TCL_LEAVE_ERR_MSG, &importNsPtr, &dummyPtr,
	    &actualCtxPtr, &simplePattern);

    if (importNsPtr == NULL) {
        Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
		"unknown namespace in namespace forget pattern \"",
		pattern, "\"", (char *) NULL);
        return TCL_ERROR;
    }

    /*
     * Scan through the command table in the source namespace and look for
     * exported commands that match the string pattern. If the current
     * namespace has an imported command that refers to one of those real
     * commands, delete it.
     */

    for (hPtr = Tcl_FirstHashEntry(&importNsPtr->cmdTable, &search);
            (hPtr != NULL);
            hPtr = Tcl_NextHashEntry(&search)) {
        cmdName = Tcl_GetHashKey(&importNsPtr->cmdTable, hPtr);
        if (Tcl_StringMatch(cmdName, simplePattern)) {
            hPtr = Tcl_FindHashEntry(&nsPtr->cmdTable, cmdName);
            if (hPtr != NULL) {	/* cmd of same name in current namespace */
                cmdPtr = (Command *) Tcl_GetHashValue(hPtr);
                if (cmdPtr->deleteProc == DeleteImportedCmd) { 
                    Tcl_DeleteCommandFromToken(interp, (Tcl_Command) cmdPtr);
                }
            }
        }
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * TclGetOriginalCommand --
 *
 *	An imported command is created in an namespace when a "real" command
 *	is imported from another namespace. If the specified command is an
 *	imported command, this procedure returns the original command it
 *	refers to. 
 *
 * Results:
 *	If the command was imported into a sequence of namespaces a, b,...,n
 *	where each successive namespace just imports the command from the
 *	previous namespace, this procedure returns the Tcl_Command token in
 *	the first namespace, a. Otherwise, if the specified command is not
 *	an imported command, the procedure returns NULL.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

Tcl_Command
TclGetOriginalCommand(command)
    Tcl_Command command;	/* The imported command for which the
				 * original command should be returned. */
{
    register Command *cmdPtr = (Command *) command;
    ImportedCmdData *dataPtr;

    if (cmdPtr->deleteProc != DeleteImportedCmd) {
	return (Tcl_Command) NULL;
    }
    
    while (cmdPtr->deleteProc == DeleteImportedCmd) {
	dataPtr = (ImportedCmdData *) cmdPtr->objClientData;
	cmdPtr = dataPtr->realCmdPtr;
    }
    return (Tcl_Command) cmdPtr;
}

/*
 *----------------------------------------------------------------------
 *
 * InvokeImportedCmd --
 *
 *	Invoked by Tcl whenever the user calls an imported command that
 *	was created by Tcl_Import. Finds the "real" command (in another
 *	namespace), and passes control to it.
 *
 * Results:
 *	Returns TCL_OK if successful, and  TCL_ERROR if anything goes wrong.
 *
 * Side effects:
 *	Returns a result in the interpreter's result object. If anything
 *	goes wrong, the result object is set to an error message.
 *
 *----------------------------------------------------------------------
 */

static int
InvokeImportedCmd(clientData, interp, objc, objv)
    ClientData clientData;	/* Points to the imported command's
				 * ImportedCmdData structure. */
    Tcl_Interp *interp;		/* Current interpreter. */
    int objc;			/* Number of arguments. */
    Tcl_Obj *CONST objv[];	/* The argument objects. */
{
    register ImportedCmdData *dataPtr = (ImportedCmdData *) clientData;
    register Command *realCmdPtr = dataPtr->realCmdPtr;

    return (*realCmdPtr->objProc)(realCmdPtr->objClientData, interp,
            objc, objv);
}

/*
 *----------------------------------------------------------------------
 *
 * DeleteImportedCmd --
 *
 *	Invoked by Tcl whenever an imported command is deleted. The "real"
 *	command keeps a list of all the imported commands that refer to it,
 *	so those imported commands can be deleted when the real command is
 *	deleted. This procedure removes the imported command reference from
 *	the real command's list, and frees up the memory associated with
 *	the imported command.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Removes the imported command from the real command's import list.
 *
 *----------------------------------------------------------------------
 */

static void
DeleteImportedCmd(clientData)
    ClientData clientData;	/* Points to the imported command's
				 * ImportedCmdData structure. */
{
    ImportedCmdData *dataPtr = (ImportedCmdData *) clientData;
    Command *realCmdPtr = dataPtr->realCmdPtr;
    Command *selfPtr = dataPtr->selfPtr;
    register ImportRef *refPtr, *prevPtr;

    prevPtr = NULL;
    for (refPtr = realCmdPtr->importRefPtr;  refPtr != NULL;
            refPtr = refPtr->nextPtr) {
	if (refPtr->importedCmdPtr == selfPtr) {
	    /*
	     * Remove *refPtr from real command's list of imported commands
	     * that refer to it.
	     */
	    
	    if (prevPtr == NULL) { /* refPtr is first in list */
		realCmdPtr->importRefPtr = refPtr->nextPtr;
	    } else {
		prevPtr->nextPtr = refPtr->nextPtr;
	    }
	    ckfree((char *) refPtr);
	    ckfree((char *) dataPtr);
	    return;
	}
	prevPtr = refPtr;
    }
	
    Tcl_Panic("DeleteImportedCmd: did not find cmd in real cmd's list of import references");
}

/*
 *----------------------------------------------------------------------
 *
 * TclGetNamespaceForQualName --
 *
 *	Given a qualified name specifying a command, variable, or namespace,
 *	and a namespace in which to resolve the name, this procedure returns
 *	a pointer to the namespace that contains the item. A qualified name
 *	consists of the "simple" name of an item qualified by the names of
 *	an arbitrary number of containing namespace separated by "::"s. If
 *	the qualified name starts with "::", it is interpreted absolutely
 *	from the global namespace. Otherwise, it is interpreted relative to
 *	the namespace specified by cxtNsPtr if it is non-NULL. If cxtNsPtr
 *	is NULL, the name is interpreted relative to the current namespace.
 *
 *	A relative name like "foo::bar::x" can be found starting in either
 *	the current namespace or in the global namespace. So each search
 *	usually follows two tracks, and two possible namespaces are
 *	returned. If the procedure sets either *nsPtrPtr or *altNsPtrPtr to
 *	NULL, then that path failed.
 *
 *	If "flags" contains TCL_GLOBAL_ONLY, the relative qualified name is
 *	sought only in the global :: namespace. The alternate search
 *	(also) starting from the global namespace is ignored and
 *	*altNsPtrPtr is set NULL. 
 *
 *	If "flags" contains TCL_NAMESPACE_ONLY, the relative qualified
 *	name is sought only in the namespace specified by cxtNsPtr. The
 *	alternate search starting from the global namespace is ignored and
 *	*altNsPtrPtr is set NULL. If both TCL_GLOBAL_ONLY and
 *	TCL_NAMESPACE_ONLY are specified, TCL_GLOBAL_ONLY is ignored and
 *	the search starts from the namespace specified by cxtNsPtr.
 *
 *	If "flags" contains CREATE_NS_IF_UNKNOWN, all namespace
 *	components of the qualified name that cannot be found are
 *	automatically created within their specified parent. This makes sure
 *	that functions like Tcl_CreateCommand always succeed. There is no
 *	alternate search path, so *altNsPtrPtr is set NULL.
 *
 *	If "flags" contains FIND_ONLY_NS, the qualified name is treated as a
 *	reference to a namespace, and the entire qualified name is
 *	followed. If the name is relative, the namespace is looked up only
 *	in the current namespace. A pointer to the namespace is stored in
 *	*nsPtrPtr and NULL is stored in *simpleNamePtr. Otherwise, if
 *	FIND_ONLY_NS is not specified, only the leading components are
 *	treated as namespace names, and a pointer to the simple name of the
 *	final component is stored in *simpleNamePtr.
 *
 * Results:
 *	It sets *nsPtrPtr and *altNsPtrPtr to point to the two possible
 *	namespaces which represent the last (containing) namespace in the
 *	qualified name. If the procedure sets either *nsPtrPtr or *altNsPtrPtr
 *	to NULL, then the search along that path failed.  The procedure also
 *	stores a pointer to the simple name of the final component in
 *	*simpleNamePtr. If the qualified name is "::" or was treated as a
 *	namespace reference (FIND_ONLY_NS), the procedure stores a pointer
 *	to the namespace in *nsPtrPtr, NULL in *altNsPtrPtr, and sets
 *	*simpleNamePtr to point to an empty string.
 *
 *	If there is an error, this procedure returns TCL_ERROR. If "flags"
 *	contains TCL_LEAVE_ERR_MSG, an error message is returned in the
 *	interpreter's result object. Otherwise, the interpreter's result
 *	object is left unchanged.
 *
 *	*actualCxtPtrPtr is set to the actual context namespace. It is
 *	set to the input context namespace pointer in cxtNsPtr. If cxtNsPtr
 *	is NULL, it is set to the current namespace context.
 *
 *	For backwards compatibility with the TclPro byte code loader,
 *	this function always returns TCL_OK.
 *
 * Side effects:
 *	If "flags" contains CREATE_NS_IF_UNKNOWN, new namespaces may be
 *	created.
 *
 *----------------------------------------------------------------------
 */

int
TclGetNamespaceForQualName(interp, qualName, cxtNsPtr, flags,
	nsPtrPtr, altNsPtrPtr, actualCxtPtrPtr, simpleNamePtr)
    Tcl_Interp *interp;		 /* Interpreter in which to find the
				  * namespace containing qualName. */
    CONST char *qualName;	 /* A namespace-qualified name of an
				  * command, variable, or namespace. */
    Namespace *cxtNsPtr;	 /* The namespace in which to start the
				  * search for qualName's namespace. If NULL
				  * start from the current namespace.
				  * Ignored if TCL_GLOBAL_ONLY or
				  * TCL_NAMESPACE_ONLY are set. */
    int flags;			 /* Flags controlling the search: an OR'd
				  * combination of TCL_GLOBAL_ONLY,
				  * TCL_NAMESPACE_ONLY,
				  * CREATE_NS_IF_UNKNOWN, and
				  * FIND_ONLY_NS. */
    Namespace **nsPtrPtr;	 /* Address where procedure stores a pointer
				  * to containing namespace if qualName is
				  * found starting from *cxtNsPtr or, if
				  * TCL_GLOBAL_ONLY is set, if qualName is
				  * found in the global :: namespace. NULL
				  * is stored otherwise. */
    Namespace **altNsPtrPtr;	 /* Address where procedure stores a pointer
				  * to containing namespace if qualName is
				  * found starting from the global ::
				  * namespace. NULL is stored if qualName
				  * isn't found starting from :: or if the
				  * TCL_GLOBAL_ONLY, TCL_NAMESPACE_ONLY,
				  * CREATE_NS_IF_UNKNOWN, FIND_ONLY_NS flag
				  * is set. */
    Namespace **actualCxtPtrPtr; /* Address where procedure stores a pointer
				  * to the actual namespace from which the
				  * search started. This is either cxtNsPtr,
				  * the :: namespace if TCL_GLOBAL_ONLY was
				  * specified, or the current namespace if
				  * cxtNsPtr was NULL. */
    CONST char **simpleNamePtr;	 /* Address where procedure stores the
				  * simple name at end of the qualName, or
				  * NULL if qualName is "::" or the flag
				  * FIND_ONLY_NS was specified. */
{
    Interp *iPtr = (Interp *) interp;
    Namespace *nsPtr = cxtNsPtr;
    Namespace *altNsPtr;
    Namespace *globalNsPtr = iPtr->globalNsPtr;
    CONST char *start, *end;
    CONST char *nsName;
    Tcl_HashEntry *entryPtr;
    Tcl_DString buffer;
    int len;

    /*
     * Determine the context namespace nsPtr in which to start the primary
     * search. If TCL_NAMESPACE_ONLY or FIND_ONLY_NS was specified, search
     * from the current namespace. If the qualName name starts with a "::"
     * or TCL_GLOBAL_ONLY was specified, search from the global
     * namespace. Otherwise, use the given namespace given in cxtNsPtr, or
     * if that is NULL, use the current namespace context. Note that we
     * always treat two or more adjacent ":"s as a namespace separator.
     */

    if (flags & (TCL_NAMESPACE_ONLY | FIND_ONLY_NS)) {
	nsPtr = (Namespace *) Tcl_GetCurrentNamespace(interp);
    } else if (flags & TCL_GLOBAL_ONLY) {
	nsPtr = globalNsPtr;
    } else if (nsPtr == NULL) {
	if (iPtr->varFramePtr != NULL) {
	    nsPtr = iPtr->varFramePtr->nsPtr;
	} else {
	    nsPtr = iPtr->globalNsPtr;
	}
    }

    start = qualName;		/* pts to start of qualifying namespace */
    if ((*qualName == ':') && (*(qualName+1) == ':')) {
	start = qualName+2;	/* skip over the initial :: */
	while (*start == ':') {
            start++;		/* skip over a subsequent : */
	}
        nsPtr = globalNsPtr;
        if (*start == '\0') {	/* qualName is just two or more ":"s */
            *nsPtrPtr        = globalNsPtr;
            *altNsPtrPtr     = NULL;
	    *actualCxtPtrPtr = globalNsPtr;
            *simpleNamePtr   = start; /* points to empty string */
            return TCL_OK;
        }
    }
    *actualCxtPtrPtr = nsPtr;

    /*
     * Start an alternate search path starting with the global namespace.
     * However, if the starting context is the global namespace, or if the
     * flag is set to search only the namespace *cxtNsPtr, ignore the
     * alternate search path.
     */

    altNsPtr = globalNsPtr;
    if ((nsPtr == globalNsPtr)
	    || (flags & (TCL_NAMESPACE_ONLY | FIND_ONLY_NS))) {
        altNsPtr = NULL;
    }

    /*
     * Loop to resolve each namespace qualifier in qualName.
     */

    Tcl_DStringInit(&buffer);
    end = start;
    while (*start != '\0') {
        /*
         * Find the next namespace qualifier (i.e., a name ending in "::")
	 * or the end of the qualified name  (i.e., a name ending in "\0").
	 * Set len to the number of characters, starting from start,
	 * in the name; set end to point after the "::"s or at the "\0".
         */

	len = 0;
        for (end = start;  *end != '\0';  end++) {
	    if ((*end == ':') && (*(end+1) == ':')) {
		end += 2;	/* skip over the initial :: */
		while (*end == ':') {
		    end++;	/* skip over the subsequent : */
		}
		break;		/* exit for loop; end is after ::'s */
	    }
            len++;
	}

	if ((*end == '\0')
	        && !((end-start >= 2) && (*(end-1) == ':') && (*(end-2) == ':'))) {
	    /*
	     * qualName ended with a simple name at start. If FIND_ONLY_NS
	     * was specified, look this up as a namespace. Otherwise,
	     * start is the name of a cmd or var and we are done.
	     */
	    
	    if (flags & FIND_ONLY_NS) {
		nsName = start;
	    } else {
		*nsPtrPtr      = nsPtr;
		*altNsPtrPtr   = altNsPtr;
		*simpleNamePtr = start;
		Tcl_DStringFree(&buffer);
		return TCL_OK;
	    }
	} else {
	    /*
	     * start points to the beginning of a namespace qualifier ending
	     * in "::". end points to the start of a name in that namespace
	     * that might be empty. Copy the namespace qualifier to a
	     * buffer so it can be null terminated. We can't modify the
	     * incoming qualName since it may be a string constant.
	     */

	    Tcl_DStringSetLength(&buffer, 0);
            Tcl_DStringAppend(&buffer, start, len);
            nsName = Tcl_DStringValue(&buffer);
        }

        /*
	 * Look up the namespace qualifier nsName in the current namespace
         * context. If it isn't found but CREATE_NS_IF_UNKNOWN is set,
         * create that qualifying namespace. This is needed for procedures
         * like Tcl_CreateCommand that cannot fail.
	 */

        if (nsPtr != NULL) {
            entryPtr = Tcl_FindHashEntry(&nsPtr->childTable, nsName);
            if (entryPtr != NULL) {
                nsPtr = (Namespace *) Tcl_GetHashValue(entryPtr);
            } else if (flags & CREATE_NS_IF_UNKNOWN) {
		Tcl_CallFrame frame;
		
		(void) Tcl_PushCallFrame(interp, &frame,
		        (Tcl_Namespace *) nsPtr, /*isProcCallFrame*/ 0);

                nsPtr = (Namespace *) Tcl_CreateNamespace(interp, nsName,
		        (ClientData) NULL, (Tcl_NamespaceDeleteProc *) NULL);
                Tcl_PopCallFrame(interp);

                if (nsPtr == NULL) {
                    Tcl_Panic("Could not create namespace '%s'", nsName);
                }
            } else {		/* namespace not found and wasn't created */
                nsPtr = NULL;
            }
        }

        /*
         * Look up the namespace qualifier in the alternate search path too.
         */

        if (altNsPtr != NULL) {
            entryPtr = Tcl_FindHashEntry(&altNsPtr->childTable, nsName);
            if (entryPtr != NULL) {
                altNsPtr = (Namespace *) Tcl_GetHashValue(entryPtr);
            } else {
                altNsPtr = NULL;
            }
        }

        /*
         * If both search paths have failed, return NULL results.
         */

        if ((nsPtr == NULL) && (altNsPtr == NULL)) {
            *nsPtrPtr      = NULL;
            *altNsPtrPtr   = NULL;
            *simpleNamePtr = NULL;
            Tcl_DStringFree(&buffer);
            return TCL_OK;
        }

	start = end;
    }

    /*
     * We ignore trailing "::"s in a namespace name, but in a command or
     * variable name, trailing "::"s refer to the cmd or var named {}.
     */

    if ((flags & FIND_ONLY_NS)
	    || ((end > start ) && (*(end-1) != ':'))) {
	*simpleNamePtr = NULL; /* found namespace name */
    } else {
	*simpleNamePtr = end;  /* found cmd/var: points to empty string */
    }

    /*
     * As a special case, if we are looking for a namespace and qualName
     * is "" and the current active namespace (nsPtr) is not the global
     * namespace, return NULL (no namespace was found). This is because
     * namespaces can not have empty names except for the global namespace.
     */

    if ((flags & FIND_ONLY_NS) && (*qualName == '\0')
	    && (nsPtr != globalNsPtr)) {
	nsPtr = NULL;
    }

    *nsPtrPtr    = nsPtr;
    *altNsPtrPtr = altNsPtr;
    Tcl_DStringFree(&buffer);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_FindNamespace --
 *
 *	Searches for a namespace.
 *
 * Results:
 *	Returns a pointer to the namespace if it is found. Otherwise,
 *	returns NULL and leaves an error message in the interpreter's
 *	result object if "flags" contains TCL_LEAVE_ERR_MSG.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

Tcl_Namespace *
Tcl_FindNamespace(interp, name, contextNsPtr, flags)
    Tcl_Interp *interp;		 /* The interpreter in which to find the
				  * namespace. */
    CONST char *name;		 /* Namespace name. If it starts with "::",
				  * will be looked up in global namespace.
				  * Else, looked up first in contextNsPtr
				  * (current namespace if contextNsPtr is
				  * NULL), then in global namespace. */
    Tcl_Namespace *contextNsPtr; /* Ignored if TCL_GLOBAL_ONLY flag is set
				  * or if the name starts with "::".
				  * Otherwise, points to namespace in which
				  * to resolve name; if NULL, look up name
				  * in the current namespace. */
    register int flags;		 /* Flags controlling namespace lookup: an
				  * OR'd combination of TCL_GLOBAL_ONLY and
				  * TCL_LEAVE_ERR_MSG flags. */
{
    Namespace *nsPtr, *dummy1Ptr, *dummy2Ptr;
    CONST char *dummy;

    /*
     * Find the namespace(s) that contain the specified namespace name.
     * Add the FIND_ONLY_NS flag to resolve the name all the way down
     * to its last component, a namespace.
     */

    TclGetNamespaceForQualName(interp, name, (Namespace *) contextNsPtr,
	    (flags | FIND_ONLY_NS), &nsPtr, &dummy1Ptr, &dummy2Ptr, &dummy);
    
    if (nsPtr != NULL) {
       return (Tcl_Namespace *) nsPtr;
    } else if (flags & TCL_LEAVE_ERR_MSG) {
	Tcl_ResetResult(interp);
	Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
                "unknown namespace \"", name, "\"", (char *) NULL);
    }
    return NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_FindCommand --
 *
 *	Searches for a command.
 *
 * Results:
 *	Returns a token for the command if it is found. Otherwise, if it
 *	can't be found or there is an error, returns NULL and leaves an
 *	error message in the interpreter's result object if "flags"
 *	contains TCL_LEAVE_ERR_MSG.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

Tcl_Command
Tcl_FindCommand(interp, name, contextNsPtr, flags)
    Tcl_Interp *interp;         /* The interpreter in which to find the
				  * command and to report errors. */
    CONST char *name;	         /* Command's name. If it starts with "::",
				  * will be looked up in global namespace.
				  * Else, looked up first in contextNsPtr
				  * (current namespace if contextNsPtr is
				  * NULL), then in global namespace. */
    Tcl_Namespace *contextNsPtr; /* Ignored if TCL_GLOBAL_ONLY flag set.
				  * Otherwise, points to namespace in which
				  * to resolve name. If NULL, look up name
				  * in the current namespace. */
    int flags;                   /* An OR'd combination of flags:
				  * TCL_GLOBAL_ONLY (look up name only in
				  * global namespace), TCL_NAMESPACE_ONLY
				  * (look up only in contextNsPtr, or the
				  * current namespace if contextNsPtr is
				  * NULL), and TCL_LEAVE_ERR_MSG. If both
				  * TCL_GLOBAL_ONLY and TCL_NAMESPACE_ONLY
				  * are given, TCL_GLOBAL_ONLY is
				  * ignored. */
{
    Interp *iPtr = (Interp*)interp;

    ResolverScheme *resPtr;
    Namespace *nsPtr[2], *cxtNsPtr;
    CONST char *simpleName;
    register Tcl_HashEntry *entryPtr;
    register Command *cmdPtr;
    register int search;
    int result;
    Tcl_Command cmd;

    /*
     * If this namespace has a command resolver, then give it first
     * crack at the command resolution.  If the interpreter has any
     * command resolvers, consult them next.  The command resolver
     * procedures may return a Tcl_Command value, they may signal
     * to continue onward, or they may signal an error.
     */
    if ((flags & TCL_GLOBAL_ONLY) != 0) {
        cxtNsPtr = (Namespace *) Tcl_GetGlobalNamespace(interp);
    }
    else if (contextNsPtr != NULL) {
        cxtNsPtr = (Namespace *) contextNsPtr;
    }
    else {
        cxtNsPtr = (Namespace *) Tcl_GetCurrentNamespace(interp);
    }

    if (cxtNsPtr->cmdResProc != NULL || iPtr->resolverPtr != NULL) {
        resPtr = iPtr->resolverPtr;

        if (cxtNsPtr->cmdResProc) {
            result = (*cxtNsPtr->cmdResProc)(interp, name,
                (Tcl_Namespace *) cxtNsPtr, flags, &cmd);
        } else {
            result = TCL_CONTINUE;
        }

        while (result == TCL_CONTINUE && resPtr) {
            if (resPtr->cmdResProc) {
                result = (*resPtr->cmdResProc)(interp, name,
                    (Tcl_Namespace *) cxtNsPtr, flags, &cmd);
            }
            resPtr = resPtr->nextPtr;
        }

        if (result == TCL_OK) {
            return cmd;
        }
        else if (result != TCL_CONTINUE) {
            return (Tcl_Command) NULL;
        }
    }

    /*
     * Find the namespace(s) that contain the command.
     */

    TclGetNamespaceForQualName(interp, name, (Namespace *) contextNsPtr,
	    flags, &nsPtr[0], &nsPtr[1], &cxtNsPtr, &simpleName);

    /*
     * Look for the command in the command table of its namespace.
     * Be sure to check both possible search paths: from the specified
     * namespace context and from the global namespace.
     */

    cmdPtr = NULL;
    for (search = 0;  (search < 2) && (cmdPtr == NULL);  search++) {
        if ((nsPtr[search] != NULL) && (simpleName != NULL)) {
	    entryPtr = Tcl_FindHashEntry(&nsPtr[search]->cmdTable,
		    simpleName);
            if (entryPtr != NULL) {
                cmdPtr = (Command *) Tcl_GetHashValue(entryPtr);
            }
        }
    }
    if (cmdPtr != NULL) {
        return (Tcl_Command) cmdPtr;
    } else if (flags & TCL_LEAVE_ERR_MSG) {
	Tcl_ResetResult(interp);
	Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
                "unknown command \"", name, "\"", (char *) NULL);
    }

    return (Tcl_Command) NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_FindNamespaceVar --
 *
 *	Searches for a namespace variable, a variable not local to a
 *	procedure. The variable can be either a scalar or an array, but
 *	may not be an element of an array.
 *
 * Results:
 *	Returns a token for the variable if it is found. Otherwise, if it
 *	can't be found or there is an error, returns NULL and leaves an
 *	error message in the interpreter's result object if "flags"
 *	contains TCL_LEAVE_ERR_MSG.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

Tcl_Var
Tcl_FindNamespaceVar(interp, name, contextNsPtr, flags)
    Tcl_Interp *interp;		 /* The interpreter in which to find the
				  * variable. */
    CONST char *name;		 /* Variable's name. If it starts with "::",
				  * will be looked up in global namespace.
				  * Else, looked up first in contextNsPtr
				  * (current namespace if contextNsPtr is
				  * NULL), then in global namespace. */
    Tcl_Namespace *contextNsPtr; /* Ignored if TCL_GLOBAL_ONLY flag set.
				  * Otherwise, points to namespace in which
				  * to resolve name. If NULL, look up name
				  * in the current namespace. */
    int flags;			 /* An OR'd combination of flags:
				  * TCL_GLOBAL_ONLY (look up name only in
				  * global namespace), TCL_NAMESPACE_ONLY
				  * (look up only in contextNsPtr, or the
				  * current namespace if contextNsPtr is
				  * NULL), and TCL_LEAVE_ERR_MSG. If both
				  * TCL_GLOBAL_ONLY and TCL_NAMESPACE_ONLY
				  * are given, TCL_GLOBAL_ONLY is
				  * ignored. */
{
    Interp *iPtr = (Interp*)interp;
    ResolverScheme *resPtr;
    Namespace *nsPtr[2], *cxtNsPtr;
    CONST char *simpleName;
    Tcl_HashEntry *entryPtr;
    Var *varPtr;
    register int search;
    int result;
    Tcl_Var var;

    /*
     * If this namespace has a variable resolver, then give it first
     * crack at the variable resolution.  It may return a Tcl_Var
     * value, it may signal to continue onward, or it may signal
     * an error.
     */
    if ((flags & TCL_GLOBAL_ONLY) != 0) {
        cxtNsPtr = (Namespace *) Tcl_GetGlobalNamespace(interp);
    }
    else if (contextNsPtr != NULL) {
        cxtNsPtr = (Namespace *) contextNsPtr;
    }
    else {
        cxtNsPtr = (Namespace *) Tcl_GetCurrentNamespace(interp);
    }

    if (cxtNsPtr->varResProc != NULL || iPtr->resolverPtr != NULL) {
        resPtr = iPtr->resolverPtr;

        if (cxtNsPtr->varResProc) {
            result = (*cxtNsPtr->varResProc)(interp, name,
                (Tcl_Namespace *) cxtNsPtr, flags, &var);
        } else {
            result = TCL_CONTINUE;
        }

        while (result == TCL_CONTINUE && resPtr) {
            if (resPtr->varResProc) {
                result = (*resPtr->varResProc)(interp, name,
                    (Tcl_Namespace *) cxtNsPtr, flags, &var);
            }
            resPtr = resPtr->nextPtr;
        }

        if (result == TCL_OK) {
            return var;
        }
        else if (result != TCL_CONTINUE) {
            return (Tcl_Var) NULL;
        }
    }

    /*
     * Find the namespace(s) that contain the variable.
     */

    TclGetNamespaceForQualName(interp, name, (Namespace *) contextNsPtr,
	    flags, &nsPtr[0], &nsPtr[1], &cxtNsPtr, &simpleName);

    /*
     * Look for the variable in the variable table of its namespace.
     * Be sure to check both possible search paths: from the specified
     * namespace context and from the global namespace.
     */

    varPtr = NULL;
    for (search = 0;  (search < 2) && (varPtr == NULL);  search++) {
        if ((nsPtr[search] != NULL) && (simpleName != NULL)) {
            entryPtr = Tcl_FindHashEntry(&nsPtr[search]->varTable,
		    simpleName);
            if (entryPtr != NULL) {
                varPtr = (Var *) Tcl_GetHashValue(entryPtr);
            }
        }
    }
    if (varPtr != NULL) {
	return (Tcl_Var) varPtr;
    } else if (flags & TCL_LEAVE_ERR_MSG) {
	Tcl_ResetResult(interp);
	Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
                "unknown variable \"", name, "\"", (char *) NULL);
    }
    return (Tcl_Var) NULL;
}

/*
 *----------------------------------------------------------------------
 *
 * TclResetShadowedCmdRefs --
 *
 *	Called when a command is added to a namespace to check for existing
 *	command references that the new command may invalidate. Consider the
 *	following cases that could happen when you add a command "foo" to a
 *	namespace "b":
 *	   1. It could shadow a command named "foo" at the global scope.
 *	      If it does, all command references in the namespace "b" are
 *	      suspect.
 *	   2. Suppose the namespace "b" resides in a namespace "a".
 *	      Then to "a" the new command "b::foo" could shadow another
 *	      command "b::foo" in the global namespace. If so, then all
 *	      command references in "a" are suspect.
 *	The same checks are applied to all parent namespaces, until we
 *	reach the global :: namespace.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	If the new command shadows an existing command, the cmdRefEpoch
 *	counter is incremented in each namespace that sees the shadow.
 *	This invalidates all command references that were previously cached
 *	in that namespace. The next time the commands are used, they are
 *	resolved from scratch.
 *
 *----------------------------------------------------------------------
 */

void
TclResetShadowedCmdRefs(interp, newCmdPtr)
    Tcl_Interp *interp;	       /* Interpreter containing the new command. */
    Command *newCmdPtr;	       /* Points to the new command. */
{
    char *cmdName;
    Tcl_HashEntry *hPtr;
    register Namespace *nsPtr;
    Namespace *trailNsPtr, *shadowNsPtr;
    Namespace *globalNsPtr = (Namespace *) Tcl_GetGlobalNamespace(interp);
    int found, i;

    /*
     * This procedure generates an array used to hold the trail list. This
     * starts out with stack-allocated space but uses dynamically-allocated
     * storage if needed.
     */

    Namespace *(trailStorage[NUM_TRAIL_ELEMS]);
    Namespace **trailPtr = trailStorage;
    int trailFront = -1;
    int trailSize = NUM_TRAIL_ELEMS;

    /*
     * Start at the namespace containing the new command, and work up
     * through the list of parents. Stop just before the global namespace,
     * since the global namespace can't "shadow" its own entries.
     *
     * The namespace "trail" list we build consists of the names of each
     * namespace that encloses the new command, in order from outermost to
     * innermost: for example, "a" then "b". Each iteration of this loop
     * eventually extends the trail upwards by one namespace, nsPtr. We use
     * this trail list to see if nsPtr (e.g. "a" in 2. above) could have
     * now-invalid cached command references. This will happen if nsPtr
     * (e.g. "a") contains a sequence of child namespaces (e.g. "b")
     * such that there is a identically-named sequence of child namespaces
     * starting from :: (e.g. "::b") whose tail namespace contains a command
     * also named cmdName.
     */

    cmdName = Tcl_GetHashKey(newCmdPtr->hPtr->tablePtr, newCmdPtr->hPtr);
    for (nsPtr = newCmdPtr->nsPtr;
	    (nsPtr != NULL) && (nsPtr != globalNsPtr);
            nsPtr = nsPtr->parentPtr) {
        /*
	 * Find the maximal sequence of child namespaces contained in nsPtr
	 * such that there is a identically-named sequence of child
	 * namespaces starting from ::. shadowNsPtr will be the tail of this
	 * sequence, or the deepest namespace under :: that might contain a
	 * command now shadowed by cmdName. We check below if shadowNsPtr
	 * actually contains a command cmdName.
	 */

        found = 1;
        shadowNsPtr = globalNsPtr;

        for (i = trailFront;  i >= 0;  i--) {
            trailNsPtr = trailPtr[i];
            hPtr = Tcl_FindHashEntry(&shadowNsPtr->childTable,
		    trailNsPtr->name);
            if (hPtr != NULL) {
                shadowNsPtr = (Namespace *) Tcl_GetHashValue(hPtr);
            } else {
                found = 0;
                break;
            }
        }

        /*
	 * If shadowNsPtr contains a command named cmdName, we invalidate
         * all of the command refs cached in nsPtr. As a boundary case,
	 * shadowNsPtr is initially :: and we check for case 1. above.
	 */

        if (found) {
            hPtr = Tcl_FindHashEntry(&shadowNsPtr->cmdTable, cmdName);
            if (hPtr != NULL) {
                nsPtr->cmdRefEpoch++;

		/* 
		 * If the shadowed command was compiled to bytecodes, we
		 * invalidate all the bytecodes in nsPtr, to force a new
		 * compilation. We use the resolverEpoch to signal the need
		 * for a fresh compilation of every bytecode.
		 */

		if ((((Command *) Tcl_GetHashValue(hPtr))->compileProc) != NULL) {
		    nsPtr->resolverEpoch++;
		}
            }
        }

        /*
	 * Insert nsPtr at the front of the trail list: i.e., at the end
	 * of the trailPtr array.
	 */

	trailFront++;
	if (trailFront == trailSize) {
	    size_t currBytes = trailSize * sizeof(Namespace *);
	    int newSize = 2*trailSize;
	    size_t newBytes = newSize * sizeof(Namespace *);
	    Namespace **newPtr =
		    (Namespace **) ckalloc((unsigned) newBytes);
	    
	    memcpy((VOID *) newPtr, (VOID *) trailPtr, currBytes);
	    if (trailPtr != trailStorage) {
		ckfree((char *) trailPtr);
	    }
	    trailPtr = newPtr;
	    trailSize = newSize;
	}
	trailPtr[trailFront] = nsPtr;
    }

    /*
     * Free any allocated storage.
     */
    
    if (trailPtr != trailStorage) {
	ckfree((char *) trailPtr);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * GetNamespaceFromObj --
 *
 *	Gets the namespace specified by the name in a Tcl_Obj.
 *
 * Results:
 *	Returns TCL_OK if the namespace was resolved successfully, and
 *	stores a pointer to the namespace in the location specified by
 *	nsPtrPtr. If the namespace can't be found, the procedure stores
 *	NULL in *nsPtrPtr and returns TCL_OK. If anything else goes wrong,
 *	this procedure returns TCL_ERROR.
 *
 * Side effects:
 *	May update the internal representation for the object, caching the
 *	namespace reference. The next time this procedure is called, the
 *	namespace value can be found quickly.
 *
 *	If anything goes wrong, an error message is left in the
 *	interpreter's result object.
 *
 *----------------------------------------------------------------------
 */

static int
GetNamespaceFromObj(interp, objPtr, nsPtrPtr)
    Tcl_Interp *interp;		/* The current interpreter. */
    Tcl_Obj *objPtr;		/* The object to be resolved as the name
				 * of a namespace. */
    Tcl_Namespace **nsPtrPtr;	/* Result namespace pointer goes here. */
{
    Interp *iPtr = (Interp *) interp;
    register ResolvedNsName *resNamePtr;
    register Namespace *nsPtr;
    Namespace *currNsPtr;
    CallFrame *savedFramePtr;
    int result = TCL_OK;
    char *name;

    /*
     * If the namespace name is fully qualified, do as if the lookup were
     * done from the global namespace; this helps avoid repeated lookups 
     * of fully qualified names. 
     */

    savedFramePtr = iPtr->varFramePtr;
    name = Tcl_GetString(objPtr);
    if ((*name++ == ':') && (*name == ':')) {
	iPtr->varFramePtr = NULL;
    }

    currNsPtr = (Namespace *) Tcl_GetCurrentNamespace(interp);
    
    /*
     * Get the internal representation, converting to a namespace type if
     * needed. The internal representation is a ResolvedNsName that points
     * to the actual namespace.
     */

    if (objPtr->typePtr != &tclNsNameType) {
        result = tclNsNameType.setFromAnyProc(interp, objPtr);
        if (result != TCL_OK) {
	    goto done;
        }
    }
    resNamePtr = (ResolvedNsName *) objPtr->internalRep.otherValuePtr;

    /*
     * Check the context namespace of the resolved symbol to make sure that
     * it is fresh. If not, then force another conversion to the namespace
     * type, to discard the old rep and create a new one. Note that we
     * verify that the namespace id of the cached namespace is the same as
     * the id when we cached it; this insures that the namespace wasn't
     * deleted and a new one created at the same address.
     */

    nsPtr = NULL;
    if ((resNamePtr != NULL)
	    && (resNamePtr->refNsPtr == currNsPtr)
	    && (resNamePtr->nsId == resNamePtr->nsPtr->nsId)) {
        nsPtr = resNamePtr->nsPtr;
	if (nsPtr->flags & NS_DEAD) {
	    nsPtr = NULL;
	}
    }
    if (nsPtr == NULL) {	/* try again */
        result = tclNsNameType.setFromAnyProc(interp, objPtr);
        if (result != TCL_OK) {
	    goto done;
        }
        resNamePtr = (ResolvedNsName *) objPtr->internalRep.otherValuePtr;
        if (resNamePtr != NULL) {
            nsPtr = resNamePtr->nsPtr;
            if (nsPtr->flags & NS_DEAD) {
                nsPtr = NULL;
            }
        }
    }
    *nsPtrPtr = (Tcl_Namespace *) nsPtr;

    done:
    iPtr->varFramePtr = savedFramePtr;
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * Tcl_NamespaceObjCmd --
 *
 *	Invoked to implement the "namespace" command that creates, deletes,
 *	or manipulates Tcl namespaces. Handles the following syntax:
 *
 *	    namespace children ?name? ?pattern?
 *	    namespace code arg
 *	    namespace current
 *	    namespace delete ?name name...?
 *	    namespace eval name arg ?arg...?
 *	    namespace exists name
 *	    namespace export ?-clear? ?pattern pattern...?
 *	    namespace forget ?pattern pattern...?
 *	    namespace import ?-force? ?pattern pattern...?
 *	    namespace inscope name arg ?arg...?
 *	    namespace origin name
 *	    namespace parent ?name?
 *	    namespace qualifiers string
 *	    namespace tail string
 *	    namespace which ?-command? ?-variable? name
 *
 * Results:
 *	Returns TCL_OK if the command is successful. Returns TCL_ERROR if
 *	anything goes wrong.
 *
 * Side effects:
 *	Based on the subcommand name (e.g., "import"), this procedure
 *	dispatches to a corresponding procedure NamespaceXXXCmd defined
 *	statically in this file. This procedure's side effects depend on
 *	whatever that subcommand procedure does. If there is an error, this
 *	procedure returns an error message in the interpreter's result
 *	object. Otherwise it may return a result in the interpreter's result
 *	object.
 *
 *----------------------------------------------------------------------
 */

int
Tcl_NamespaceObjCmd(clientData, interp, objc, objv)
    ClientData clientData;		/* Arbitrary value passed to cmd. */
    Tcl_Interp *interp;			/* Current interpreter. */
    register int objc;			/* Number of arguments. */
    register Tcl_Obj *CONST objv[];	/* Argument objects. */
{
    static CONST char *subCmds[] = {
	"children", "code", "current", "delete", "ensemble",
	"eval", "exists", "export", "forget", "import",
	"inscope", "origin", "parent", "qualifiers",
	"tail", "which", (char *) NULL
    };
    enum NSSubCmdIdx {
	NSChildrenIdx, NSCodeIdx, NSCurrentIdx, NSDeleteIdx, NSEnsembleIdx,
	NSEvalIdx, NSExistsIdx, NSExportIdx, NSForgetIdx, NSImportIdx,
	NSInscopeIdx, NSOriginIdx, NSParentIdx, NSQualifiersIdx,
	NSTailIdx, NSWhichIdx
    };
    int index, result;

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "subcommand ?arg ...?");
        return TCL_ERROR;
    }

    /*
     * Return an index reflecting the particular subcommand.
     */

    result = Tcl_GetIndexFromObj((Tcl_Interp *) interp, objv[1], subCmds,
	    "option", /*flags*/ 0, (int *) &index);
    if (result != TCL_OK) {
	return result;
    }
    
    switch (index) {
        case NSChildrenIdx:
	    result = NamespaceChildrenCmd(clientData, interp, objc, objv);
            break;
        case NSCodeIdx:
	    result = NamespaceCodeCmd(clientData, interp, objc, objv);
            break;
        case NSCurrentIdx:
	    result = NamespaceCurrentCmd(clientData, interp, objc, objv);
            break;
        case NSDeleteIdx:
	    result = NamespaceDeleteCmd(clientData, interp, objc, objv);
            break;
        case NSEnsembleIdx:
	    result = NamespaceEnsembleCmd(clientData, interp, objc, objv);
            break;
        case NSEvalIdx:
	    result = NamespaceEvalCmd(clientData, interp, objc, objv);
            break;
        case NSExistsIdx:
	    result = NamespaceExistsCmd(clientData, interp, objc, objv);
            break;
        case NSExportIdx:
	    result = NamespaceExportCmd(clientData, interp, objc, objv);
            break;
        case NSForgetIdx:
	    result = NamespaceForgetCmd(clientData, interp, objc, objv);
            break;
        case NSImportIdx:
	    result = NamespaceImportCmd(clientData, interp, objc, objv);
            break;
        case NSInscopeIdx:
	    result = NamespaceInscopeCmd(clientData, interp, objc, objv);
            break;
        case NSOriginIdx:
	    result = NamespaceOriginCmd(clientData, interp, objc, objv);
            break;
        case NSParentIdx:
	    result = NamespaceParentCmd(clientData, interp, objc, objv);
            break;
        case NSQualifiersIdx:
	    result = NamespaceQualifiersCmd(clientData, interp, objc, objv);
            break;
        case NSTailIdx:
	    result = NamespaceTailCmd(clientData, interp, objc, objv);
            break;
        case NSWhichIdx:
	    result = NamespaceWhichCmd(clientData, interp, objc, objv);
            break;
    }
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * NamespaceChildrenCmd --
 *
 *	Invoked to implement the "namespace children" command that returns a
 *	list containing the fully-qualified names of the child namespaces of
 *	a given namespace. Handles the following syntax:
 *
 *	    namespace children ?name? ?pattern?
 *
 * Results:
 *	Returns TCL_OK if successful, and TCL_ERROR if anything goes wrong.
 *
 * Side effects:
 *	Returns a result in the interpreter's result object. If anything
 *	goes wrong, the result is an error message.
 *
 *----------------------------------------------------------------------
 */

static int
NamespaceChildrenCmd(dummy, interp, objc, objv)
    ClientData dummy;		/* Not used. */
    Tcl_Interp *interp;		/* Current interpreter. */
    int objc;			/* Number of arguments. */
    Tcl_Obj *CONST objv[];	/* Argument objects. */
{
    Tcl_Namespace *namespacePtr;
    Namespace *nsPtr, *childNsPtr;
    Namespace *globalNsPtr = (Namespace *) Tcl_GetGlobalNamespace(interp);
    char *pattern = NULL;
    Tcl_DString buffer;
    register Tcl_HashEntry *entryPtr;
    Tcl_HashSearch search;
    Tcl_Obj *listPtr, *elemPtr;

    /*
     * Get a pointer to the specified namespace, or the current namespace.
     */

    if (objc == 2) {
	nsPtr = (Namespace *) Tcl_GetCurrentNamespace(interp);
    } else if ((objc == 3) || (objc == 4)) {
        if (GetNamespaceFromObj(interp, objv[2], &namespacePtr) != TCL_OK) {
            return TCL_ERROR;
        }
        if (namespacePtr == NULL) {
	    Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
                    "unknown namespace \"", Tcl_GetString(objv[2]),
		    "\" in namespace children command", (char *) NULL);
            return TCL_ERROR;
        }
        nsPtr = (Namespace *) namespacePtr;
    } else {
	Tcl_WrongNumArgs(interp, 2, objv, "?name? ?pattern?");
        return TCL_ERROR;
    }

    /*
     * Get the glob-style pattern, if any, used to narrow the search.
     */

    Tcl_DStringInit(&buffer);
    if (objc == 4) {
        char *name = Tcl_GetString(objv[3]);
	
        if ((*name == ':') && (*(name+1) == ':')) {
            pattern = name;
        } else {
            Tcl_DStringAppend(&buffer, nsPtr->fullName, -1);
            if (nsPtr != globalNsPtr) {
                Tcl_DStringAppend(&buffer, "::", 2);
            }
            Tcl_DStringAppend(&buffer, name, -1);
            pattern = Tcl_DStringValue(&buffer);
        }
    }

    /*
     * Create a list containing the full names of all child namespaces
     * whose names match the specified pattern, if any.
     */

    listPtr = Tcl_NewListObj(0, (Tcl_Obj **) NULL);
    entryPtr = Tcl_FirstHashEntry(&nsPtr->childTable, &search);
    while (entryPtr != NULL) {
        childNsPtr = (Namespace *) Tcl_GetHashValue(entryPtr);
        if ((pattern == NULL)
	        || Tcl_StringMatch(childNsPtr->fullName, pattern)) {
            elemPtr = Tcl_NewStringObj(childNsPtr->fullName, -1);
            Tcl_ListObjAppendElement(interp, listPtr, elemPtr);
        }
        entryPtr = Tcl_NextHashEntry(&search);
    }

    Tcl_SetObjResult(interp, listPtr);
    Tcl_DStringFree(&buffer);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * NamespaceCodeCmd --
 *
 *	Invoked to implement the "namespace code" command to capture the
 *	namespace context of a command. Handles the following syntax:
 *
 *	    namespace code arg
 *
 *	Here "arg" can be a list. "namespace code arg" produces a result
 *	equivalent to that produced by the command
 *
 *	    list ::namespace inscope [namespace current] $arg
 *
 *	However, if "arg" is itself a scoped value starting with
 *	"::namespace inscope", then the result is just "arg".
 *
 * Results:
 *	Returns TCL_OK if successful, and TCL_ERROR if anything goes wrong.
 *
 * Side effects:
 *	If anything goes wrong, this procedure returns an error
 *	message as the result in the interpreter's result object.
 *
 *----------------------------------------------------------------------
 */

static int
NamespaceCodeCmd(dummy, interp, objc, objv)
    ClientData dummy;		/* Not used. */
    Tcl_Interp *interp;		/* Current interpreter. */
    int objc;			/* Number of arguments. */
    Tcl_Obj *CONST objv[];	/* Argument objects. */
{
    Namespace *currNsPtr;
    Tcl_Obj *listPtr, *objPtr;
    register char *arg, *p;
    int length;

    if (objc != 3) {
	Tcl_WrongNumArgs(interp, 2, objv, "arg");
        return TCL_ERROR;
    }

    /*
     * If "arg" is already a scoped value, then return it directly.
     */

    arg = Tcl_GetStringFromObj(objv[2], &length);
    while (*arg == ':') { 
	arg++; 
	length--; 
    } 
    if ((*arg == 'n') && (length > 17)
	    && (strncmp(arg, "namespace", 9) == 0)) {
	for (p = (arg + 9);  (*p == ' ');  p++) {
	    /* empty body: skip over spaces */
	}
	if ((*p == 'i') && ((p + 7) <= (arg + length))
	        && (strncmp(p, "inscope", 7) == 0)) {
	    Tcl_SetObjResult(interp, objv[2]);
	    return TCL_OK;
	}
    }

    /*
     * Otherwise, construct a scoped command by building a list with
     * "namespace inscope", the full name of the current namespace, and 
     * the argument "arg". By constructing a list, we ensure that scoped
     * commands are interpreted properly when they are executed later,
     * by the "namespace inscope" command.
     */

    listPtr = Tcl_NewListObj(0, (Tcl_Obj **) NULL);
    Tcl_ListObjAppendElement(interp, listPtr,
            Tcl_NewStringObj("::namespace", -1));
    Tcl_ListObjAppendElement(interp, listPtr,
	    Tcl_NewStringObj("inscope", -1));

    currNsPtr = (Namespace *) Tcl_GetCurrentNamespace(interp);
    if (currNsPtr == (Namespace *) Tcl_GetGlobalNamespace(interp)) {
	objPtr = Tcl_NewStringObj("::", -1);
    } else {
	objPtr = Tcl_NewStringObj(currNsPtr->fullName, -1);
    }
    Tcl_ListObjAppendElement(interp, listPtr, objPtr);
    
    Tcl_ListObjAppendElement(interp, listPtr, objv[2]);

    Tcl_SetObjResult(interp, listPtr);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * NamespaceCurrentCmd --
 *
 *	Invoked to implement the "namespace current" command which returns
 *	the fully-qualified name of the current namespace. Handles the
 *	following syntax:
 *
 *	    namespace current
 *
 * Results:
 *	Returns TCL_OK if successful, and TCL_ERROR if anything goes wrong.
 *
 * Side effects:
 *	Returns a result in the interpreter's result object. If anything
 *	goes wrong, the result is an error message.
 *
 *----------------------------------------------------------------------
 */

static int
NamespaceCurrentCmd(dummy, interp, objc, objv)
    ClientData dummy;		/* Not used. */
    Tcl_Interp *interp;		/* Current interpreter. */
    int objc;			/* Number of arguments. */
    Tcl_Obj *CONST objv[];	/* Argument objects. */
{
    register Namespace *currNsPtr;

    if (objc != 2) {
	Tcl_WrongNumArgs(interp, 2, objv, NULL);
        return TCL_ERROR;
    }

    /*
     * The "real" name of the global namespace ("::") is the null string,
     * but we return "::" for it as a convenience to programmers. Note that
     * "" and "::" are treated as synonyms by the namespace code so that it
     * is still easy to do things like:
     *
     *    namespace [namespace current]::bar { ... }
     */

    currNsPtr = (Namespace *) Tcl_GetCurrentNamespace(interp);
    if (currNsPtr == (Namespace *) Tcl_GetGlobalNamespace(interp)) {
        Tcl_AppendToObj(Tcl_GetObjResult(interp), "::", -1);
    } else {
	Tcl_AppendToObj(Tcl_GetObjResult(interp), currNsPtr->fullName, -1);
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * NamespaceDeleteCmd --
 *
 *	Invoked to implement the "namespace delete" command to delete
 *	namespace(s). Handles the following syntax:
 *
 *	    namespace delete ?name name...?
 *
 *	Each name identifies a namespace. It may include a sequence of
 *	namespace qualifiers separated by "::"s. If a namespace is found, it
 *	is deleted: all variables and procedures contained in that namespace
 *	are deleted. If that namespace is being used on the call stack, it
 *	is kept alive (but logically deleted) until it is removed from the
 *	call stack: that is, it can no longer be referenced by name but any
 *	currently executing procedure that refers to it is allowed to do so
 *	until the procedure returns. If the namespace can't be found, this
 *	procedure returns an error. If no namespaces are specified, this
 *	command does nothing.
 *
 * Results:
 *	Returns TCL_OK if successful, and  TCL_ERROR if anything goes wrong.
 *
 * Side effects:
 *	Deletes the specified namespaces. If anything goes wrong, this
 *	procedure returns an error message in the interpreter's
 *	result object.
 *
 *----------------------------------------------------------------------
 */

static int
NamespaceDeleteCmd(dummy, interp, objc, objv)
    ClientData dummy;		/* Not used. */
    Tcl_Interp *interp;		/* Current interpreter. */
    int objc;			/* Number of arguments. */
    Tcl_Obj *CONST objv[];	/* Argument objects. */
{
    Tcl_Namespace *namespacePtr;
    char *name;
    register int i;

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 2, objv, "?name name...?");
        return TCL_ERROR;
    }

    /*
     * Destroying one namespace may cause another to be destroyed. Break
     * this into two passes: first check to make sure that all namespaces on
     * the command line are valid, and report any errors.
     */

    for (i = 2;  i < objc;  i++) {
        name = Tcl_GetString(objv[i]);
	namespacePtr = Tcl_FindNamespace(interp, name,
		(Tcl_Namespace *) NULL, /*flags*/ 0);
        if (namespacePtr == NULL) {
	    Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
                    "unknown namespace \"", Tcl_GetString(objv[i]),
		    "\" in namespace delete command", (char *) NULL);
            return TCL_ERROR;
        }
    }

    /*
     * Okay, now delete each namespace.
     */

    for (i = 2;  i < objc;  i++) {
        name = Tcl_GetString(objv[i]);
	namespacePtr = Tcl_FindNamespace(interp, name,
	    (Tcl_Namespace *) NULL, /* flags */ 0);
	if (namespacePtr) {
            Tcl_DeleteNamespace(namespacePtr);
        }
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * NamespaceEvalCmd --
 *
 *	Invoked to implement the "namespace eval" command. Executes
 *	commands in a namespace. If the namespace does not already exist,
 *	it is created. Handles the following syntax:
 *
 *	    namespace eval name arg ?arg...?
 *
 *	If more than one arg argument is specified, the command that is
 *	executed is the result of concatenating the arguments together with
 *	a space between each argument.
 *
 * Results:
 *	Returns TCL_OK if the namespace is found and the commands are
 *	executed successfully. Returns TCL_ERROR if anything goes wrong.
 *
 * Side effects:
 *	Returns the result of the command in the interpreter's result
 *	object. If anything goes wrong, this procedure returns an error
 *	message as the result.
 *
 *----------------------------------------------------------------------
 */

static int
NamespaceEvalCmd(dummy, interp, objc, objv)
    ClientData dummy;		/* Not used. */
    Tcl_Interp *interp;		/* Current interpreter. */
    int objc;			/* Number of arguments. */
    Tcl_Obj *CONST objv[];	/* Argument objects. */
{
    Tcl_Namespace *namespacePtr;
    CallFrame frame;
    Tcl_Obj *objPtr;
    char *name;
    int length, result;

    if (objc < 4) {
        Tcl_WrongNumArgs(interp, 2, objv, "name arg ?arg...?");
        return TCL_ERROR;
    }

    /*
     * Try to resolve the namespace reference, caching the result in the
     * namespace object along the way.
     */

    result = GetNamespaceFromObj(interp, objv[2], &namespacePtr);
    if (result != TCL_OK) {
        return result;
    }

    /*
     * If the namespace wasn't found, try to create it.
     */
    
    if (namespacePtr == NULL) {
	name = Tcl_GetStringFromObj(objv[2], &length);
	namespacePtr = Tcl_CreateNamespace(interp, name, (ClientData) NULL, 
                (Tcl_NamespaceDeleteProc *) NULL);
	if (namespacePtr == NULL) {
	    return TCL_ERROR;
	}
    }

    /*
     * Make the specified namespace the current namespace and evaluate
     * the command(s).
     */

    result = Tcl_PushCallFrame(interp, (Tcl_CallFrame *) &frame, 
            namespacePtr, /*isProcCallFrame*/ 0);
    if (result != TCL_OK) {
        return TCL_ERROR;
    }
    frame.objc = objc;
    frame.objv = objv;  /* ref counts do not need to be incremented here */

    if (objc == 4) {
        result = Tcl_EvalObjEx(interp, objv[3], 0);
    } else {
	/*
	 * More than one argument: concatenate them together with spaces
	 * between, then evaluate the result.  Tcl_EvalObjEx will delete
	 * the object when it decrements its refcount after eval'ing it.
	 */
        objPtr = Tcl_ConcatObj(objc-3, objv+3);
        result = Tcl_EvalObjEx(interp, objPtr, TCL_EVAL_DIRECT);
    }
    if (result == TCL_ERROR) {
	Tcl_Obj *errorLine = Tcl_NewIntObj(interp->errorLine);
	Tcl_Obj *msg = Tcl_NewStringObj("\n    (in namespace eval \"", -1);
	Tcl_IncrRefCount(errorLine);
	Tcl_IncrRefCount(msg);
	TclAppendLimitedToObj(msg, namespacePtr->fullName, -1, 200, "");
	Tcl_AppendToObj(msg, "\" script line ", -1);
	Tcl_AppendObjToObj(msg, errorLine);
	Tcl_DecrRefCount(errorLine);
	Tcl_AppendToObj(msg, ")", -1);
        TclAppendObjToErrorInfo(interp, msg);
	Tcl_DecrRefCount(msg);
    }

    /*
     * Restore the previous "current" namespace.
     */
    
    Tcl_PopCallFrame(interp);
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * NamespaceExistsCmd --
 *
 *	Invoked to implement the "namespace exists" command that returns 
 *	true if the given namespace currently exists, and false otherwise.
 *	Handles the following syntax:
 *
 *	    namespace exists name
 *
 * Results:
 *	Returns TCL_OK if successful, and TCL_ERROR if anything goes wrong.
 *
 * Side effects:
 *	Returns a result in the interpreter's result object. If anything
 *	goes wrong, the result is an error message.
 *
 *----------------------------------------------------------------------
 */

static int
NamespaceExistsCmd(dummy, interp, objc, objv)
    ClientData dummy;		/* Not used. */
    Tcl_Interp *interp;		/* Current interpreter. */
    int objc;			/* Number of arguments. */
    Tcl_Obj *CONST objv[];	/* Argument objects. */
{
    Tcl_Namespace *namespacePtr;

    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 2, objv, "name");
        return TCL_ERROR;
    }

    /*
     * Check whether the given namespace exists
     */

    if (GetNamespaceFromObj(interp, objv[2], &namespacePtr) != TCL_OK) {
        return TCL_ERROR;
    }

    Tcl_SetBooleanObj(Tcl_GetObjResult(interp), (namespacePtr != NULL));
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * NamespaceExportCmd --
 *
 *	Invoked to implement the "namespace export" command that specifies
 *	which commands are exported from a namespace. The exported commands
 *	are those that can be imported into another namespace using
 *	"namespace import". Both commands defined in a namespace and
 *	commands the namespace has imported can be exported by a
 *	namespace. This command has the following syntax:
 *
 *	    namespace export ?-clear? ?pattern pattern...?
 *
 *	Each pattern may contain "string match"-style pattern matching
 *	special characters, but the pattern may not include any namespace
 *	qualifiers: that is, the pattern must specify commands in the
 *	current (exporting) namespace. The specified patterns are appended
 *	onto the namespace's list of export patterns.
 *
 *	To reset the namespace's export pattern list, specify the "-clear"
 *	flag.
 *
 *	If there are no export patterns and the "-clear" flag isn't given,
 *	this command returns the namespace's current export list.
 *
 * Results:
 *	Returns TCL_OK if successful, and TCL_ERROR if anything goes wrong.
 *
 * Side effects:
 *	Returns a result in the interpreter's result object. If anything
 *	goes wrong, the result is an error message.
 *
 *----------------------------------------------------------------------
 */

static int
NamespaceExportCmd(dummy, interp, objc, objv)
    ClientData dummy;		/* Not used. */
    Tcl_Interp *interp;		/* Current interpreter. */
    int objc;			/* Number of arguments. */
    Tcl_Obj *CONST objv[];	/* Argument objects. */
{
    Namespace *currNsPtr = (Namespace*) Tcl_GetCurrentNamespace(interp);
    char *pattern, *string;
    int resetListFirst = 0;
    int firstArg, patternCt, i, result;

    if (objc < 2) {
	Tcl_WrongNumArgs(interp, 2, objv,
	        "?-clear? ?pattern pattern...?");
        return TCL_ERROR;
    }

    /*
     * Process the optional "-clear" argument.
     */

    firstArg = 2;
    if (firstArg < objc) {
	string = Tcl_GetString(objv[firstArg]);
	if (strcmp(string, "-clear") == 0) {
	    resetListFirst = 1;
	    firstArg++;
	}
    }

    /*
     * If no pattern arguments are given, and "-clear" isn't specified,
     * return the namespace's current export pattern list.
     */

    patternCt = (objc - firstArg);
    if (patternCt == 0) {
	if (firstArg > 2) {
	    return TCL_OK;
	} else {		/* create list with export patterns */
	    Tcl_Obj *listPtr = Tcl_NewListObj(0, (Tcl_Obj **) NULL);
	    result = Tcl_AppendExportList(interp,
		    (Tcl_Namespace *) currNsPtr, listPtr);
	    if (result != TCL_OK) {
		return result;
	    }
	    Tcl_SetObjResult(interp, listPtr);
	    return TCL_OK;
	}
    }

    /*
     * Add each pattern to the namespace's export pattern list.
     */
    
    for (i = firstArg;  i < objc;  i++) {
	pattern = Tcl_GetString(objv[i]);
	result = Tcl_Export(interp, (Tcl_Namespace *) currNsPtr, pattern,
		((i == firstArg)? resetListFirst : 0));
        if (result != TCL_OK) {
            return result;
        }
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * NamespaceForgetCmd --
 *
 *	Invoked to implement the "namespace forget" command to remove
 *	imported commands from a namespace. Handles the following syntax:
 *
 *	    namespace forget ?pattern pattern...?
 *
 *	Each pattern is a name like "foo::*" or "a::b::x*". That is, the
 *	pattern may include the special pattern matching characters
 *	recognized by the "string match" command, but only in the command
 *	name at the end of the qualified name; the special pattern
 *	characters may not appear in a namespace name. All of the commands
 *	that match that pattern are checked to see if they have an imported
 *	command in the current namespace that refers to the matched
 *	command. If there is an alias, it is removed.
 *	
 * Results:
 *	Returns TCL_OK if successful, and TCL_ERROR if anything goes wrong.
 *
 * Side effects:
 *	Imported commands are removed from the current namespace. If
 *	anything goes wrong, this procedure returns an error message in the
 *	interpreter's result object.
 *
 *----------------------------------------------------------------------
 */

static int
NamespaceForgetCmd(dummy, interp, objc, objv)
    ClientData dummy;		/* Not used. */
    Tcl_Interp *interp;		/* Current interpreter. */
    int objc;			/* Number of arguments. */
    Tcl_Obj *CONST objv[];	/* Argument objects. */
{
    char *pattern;
    register int i, result;

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 2, objv, "?pattern pattern...?");
        return TCL_ERROR;
    }

    for (i = 2;  i < objc;  i++) {
        pattern = Tcl_GetString(objv[i]);
	result = Tcl_ForgetImport(interp, (Tcl_Namespace *) NULL, pattern);
        if (result != TCL_OK) {
            return result;
        }
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * NamespaceImportCmd --
 *
 *	Invoked to implement the "namespace import" command that imports
 *	commands into a namespace. Handles the following syntax:
 *
 *	    namespace import ?-force? ?pattern pattern...?
 *
 *	Each pattern is a namespace-qualified name like "foo::*",
 *	"a::b::x*", or "bar::p". That is, the pattern may include the
 *	special pattern matching characters recognized by the "string match"
 *	command, but only in the command name at the end of the qualified
 *	name; the special pattern characters may not appear in a namespace
 *	name. All of the commands that match the pattern and which are
 *	exported from their namespace are made accessible from the current
 *	namespace context. This is done by creating a new "imported command"
 *	in the current namespace that points to the real command in its
 *	original namespace; when the imported command is called, it invokes
 *	the real command.
 *
 *	If an imported command conflicts with an existing command, it is
 *	treated as an error. But if the "-force" option is included, then
 *	existing commands are overwritten by the imported commands.
 *	
 * Results:
 *	Returns TCL_OK if successful, and TCL_ERROR if anything goes wrong.
 *
 * Side effects:
 *	Adds imported commands to the current namespace. If anything goes
 *	wrong, this procedure returns an error message in the interpreter's
 *	result object.
 *
 *----------------------------------------------------------------------
 */

static int
NamespaceImportCmd(dummy, interp, objc, objv)
    ClientData dummy;		/* Not used. */
    Tcl_Interp *interp;		/* Current interpreter. */
    int objc;			/* Number of arguments. */
    Tcl_Obj *CONST objv[];	/* Argument objects. */
{
    int allowOverwrite = 0;
    char *string, *pattern;
    register int i, result;
    int firstArg;

    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 2, objv,
	        "?-force? ?pattern pattern...?");
        return TCL_ERROR;
    }

    /*
     * Skip over the optional "-force" as the first argument.
     */

    firstArg = 2;
    if (firstArg < objc) {
	string = Tcl_GetString(objv[firstArg]);
	if ((*string == '-') && (strcmp(string, "-force") == 0)) {
	    allowOverwrite = 1;
	    firstArg++;
	}
    }

    /*
     * Handle the imports for each of the patterns.
     */

    for (i = firstArg;  i < objc;  i++) {
        pattern = Tcl_GetString(objv[i]);
	result = Tcl_Import(interp, (Tcl_Namespace *) NULL, pattern,
	        allowOverwrite);
        if (result != TCL_OK) {
            return result;
        }
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * NamespaceInscopeCmd --
 *
 *	Invoked to implement the "namespace inscope" command that executes a
 *	script in the context of a particular namespace. This command is not
 *	expected to be used directly by programmers; calls to it are
 *	generated implicitly when programs use "namespace code" commands
 *	to register callback scripts. Handles the following syntax:
 *
 *	    namespace inscope name arg ?arg...?
 *
 *	The "namespace inscope" command is much like the "namespace eval"
 *	command except that it has lappend semantics and the namespace must
 *	already exist. It treats the first argument as a list, and appends
 *	any arguments after the first onto the end as proper list elements.
 *	For example,
 *
 *	    namespace inscope ::foo a b c d
 *
 *	is equivalent to
 *
 *	    namespace eval ::foo [concat a [list b c d]]
 *
 *	This lappend semantics is important because many callback scripts
 *	are actually prefixes.
 *
 * Results:
 *	Returns TCL_OK to indicate success, or TCL_ERROR to indicate
 *	failure.
 *
 * Side effects:
 *	Returns a result in the Tcl interpreter's result object.
 *
 *----------------------------------------------------------------------
 */

static int
NamespaceInscopeCmd(dummy, interp, objc, objv)
    ClientData dummy;		/* Not used. */
    Tcl_Interp *interp;		/* Current interpreter. */
    int objc;			/* Number of arguments. */
    Tcl_Obj *CONST objv[];	/* Argument objects. */
{
    Tcl_Namespace *namespacePtr;
    Tcl_CallFrame frame;
    int i, result;

    if (objc < 4) {
	Tcl_WrongNumArgs(interp, 2, objv, "name arg ?arg...?");
        return TCL_ERROR;
    }

    /*
     * Resolve the namespace reference.
     */

    result = GetNamespaceFromObj(interp, objv[2], &namespacePtr);
    if (result != TCL_OK) {
        return result;
    }
    if (namespacePtr == NULL) {
	Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
	        "unknown namespace \"", Tcl_GetString(objv[2]),
		"\" in inscope namespace command", (char *) NULL);
        return TCL_ERROR;
    }

    /*
     * Make the specified namespace the current namespace.
     */

    result = Tcl_PushCallFrame(interp, &frame, namespacePtr,
	    /*isProcCallFrame*/ 0);
    if (result != TCL_OK) {
        return result;
    }

    /*
     * Execute the command. If there is just one argument, just treat it as
     * a script and evaluate it. Otherwise, create a list from the arguments
     * after the first one, then concatenate the first argument and the list
     * of extra arguments to form the command to evaluate.
     */

    if (objc == 4) {
        result = Tcl_EvalObjEx(interp, objv[3], 0);
    } else {
	Tcl_Obj *concatObjv[2];
	register Tcl_Obj *listPtr, *cmdObjPtr;
	
        listPtr = Tcl_NewListObj(0, (Tcl_Obj **) NULL);
        for (i = 4;  i < objc;  i++) {
	    result = Tcl_ListObjAppendElement(interp, listPtr, objv[i]);
            if (result != TCL_OK) {
                Tcl_DecrRefCount(listPtr); /* free unneeded obj */
                return result;
            }
        }

	concatObjv[0] = objv[3];
	concatObjv[1] = listPtr;
	cmdObjPtr = Tcl_ConcatObj(2, concatObjv);
        result = Tcl_EvalObjEx(interp, cmdObjPtr, TCL_EVAL_DIRECT);
	Tcl_DecrRefCount(listPtr);    /* we're done with the list object */
    }
    if (result == TCL_ERROR) {
        char msg[256 + TCL_INTEGER_SPACE];
	
        sprintf(msg,
	    "\n    (in namespace inscope \"%.200s\" script line %d)",
            namespacePtr->fullName, interp->errorLine);
        Tcl_AddObjErrorInfo(interp, msg, -1);
    }

    /*
     * Restore the previous "current" namespace.
     */

    Tcl_PopCallFrame(interp);
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * NamespaceOriginCmd --
 *
 *	Invoked to implement the "namespace origin" command to return the
 *	fully-qualified name of the "real" command to which the specified
 *	"imported command" refers. Handles the following syntax:
 *
 *	    namespace origin name
 *
 * Results:
 *	An imported command is created in an namespace when that namespace
 *	imports a command from another namespace. If a command is imported
 *	into a sequence of namespaces a, b,...,n where each successive
 *	namespace just imports the command from the previous namespace, this
 *	command returns the fully-qualified name of the original command in
 *	the first namespace, a. If "name" does not refer to an alias, its
 *	fully-qualified name is returned. The returned name is stored in the
 *	interpreter's result object. This procedure returns TCL_OK if
 *	successful, and TCL_ERROR if anything goes wrong.
 *
 * Side effects:
 *	If anything goes wrong, this procedure returns an error message in
 *	the interpreter's result object.
 *
 *----------------------------------------------------------------------
 */

static int
NamespaceOriginCmd(dummy, interp, objc, objv)
    ClientData dummy;		/* Not used. */
    Tcl_Interp *interp;		/* Current interpreter. */
    int objc;			/* Number of arguments. */
    Tcl_Obj *CONST objv[];	/* Argument objects. */
{
    Tcl_Command command, origCommand;

    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 2, objv, "name");
        return TCL_ERROR;
    }

    command = Tcl_GetCommandFromObj(interp, objv[2]);
    if (command == (Tcl_Command) NULL) {
	Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
		"invalid command name \"", Tcl_GetString(objv[2]),
		"\"", (char *) NULL);
	return TCL_ERROR;
    }
    origCommand = TclGetOriginalCommand(command);
    if (origCommand == (Tcl_Command) NULL) {
	/*
	 * The specified command isn't an imported command. Return the
	 * command's name qualified by the full name of the namespace it
	 * was defined in.
	 */
	
	Tcl_GetCommandFullName(interp, command, Tcl_GetObjResult(interp));
    } else {
	Tcl_GetCommandFullName(interp, origCommand, Tcl_GetObjResult(interp));
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * NamespaceParentCmd --
 *
 *	Invoked to implement the "namespace parent" command that returns the
 *	fully-qualified name of the parent namespace for a specified
 *	namespace. Handles the following syntax:
 *
 *	    namespace parent ?name?
 *
 * Results:
 *	Returns TCL_OK if successful, and TCL_ERROR if anything goes wrong.
 *
 * Side effects:
 *	Returns a result in the interpreter's result object. If anything
 *	goes wrong, the result is an error message.
 *
 *----------------------------------------------------------------------
 */

static int
NamespaceParentCmd(dummy, interp, objc, objv)
    ClientData dummy;		/* Not used. */
    Tcl_Interp *interp;		/* Current interpreter. */
    int objc;			/* Number of arguments. */
    Tcl_Obj *CONST objv[];	/* Argument objects. */
{
    Tcl_Namespace *nsPtr;
    int result;

    if (objc == 2) {
        nsPtr = Tcl_GetCurrentNamespace(interp);
    } else if (objc == 3) {
	result = GetNamespaceFromObj(interp, objv[2], &nsPtr);
        if (result != TCL_OK) {
            return result;
        }
        if (nsPtr == NULL) {
            Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
                    "unknown namespace \"", Tcl_GetString(objv[2]),
		    "\" in namespace parent command", (char *) NULL);
            return TCL_ERROR;
        }
    } else {
        Tcl_WrongNumArgs(interp, 2, objv, "?name?");
        return TCL_ERROR;
    }

    /*
     * Report the parent of the specified namespace.
     */

    if (nsPtr->parentPtr != NULL) {
        Tcl_SetStringObj(Tcl_GetObjResult(interp),
	        nsPtr->parentPtr->fullName, -1);
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * NamespaceQualifiersCmd --
 *
 *	Invoked to implement the "namespace qualifiers" command that returns
 *	any leading namespace qualifiers in a string. These qualifiers are
 *	namespace names separated by "::"s. For example, for "::foo::p" this
 *	command returns "::foo", and for "::" it returns "". This command
 *	is the complement of the "namespace tail" command. Note that this
 *	command does not check whether the "namespace" names are, in fact,
 *	the names of currently defined namespaces. Handles the following
 *	syntax:
 *
 *	    namespace qualifiers string
 *
 * Results:
 *	Returns TCL_OK if successful, and  TCL_ERROR if anything goes wrong.
 *
 * Side effects:
 *	Returns a result in the interpreter's result object. If anything
 *	goes wrong, the result is an error message.
 *
 *----------------------------------------------------------------------
 */

static int
NamespaceQualifiersCmd(dummy, interp, objc, objv)
    ClientData dummy;		/* Not used. */
    Tcl_Interp *interp;		/* Current interpreter. */
    int objc;			/* Number of arguments. */
    Tcl_Obj *CONST objv[];	/* Argument objects. */
{
    register char *name, *p;
    int length;

    if (objc != 3) {
	Tcl_WrongNumArgs(interp, 2, objv, "string");
        return TCL_ERROR;
    }

    /*
     * Find the end of the string, then work backward and find
     * the start of the last "::" qualifier.
     */

    name = Tcl_GetString(objv[2]);
    for (p = name;  *p != '\0';  p++) {
	/* empty body */
    }
    while (--p >= name) {
        if ((*p == ':') && (p > name) && (*(p-1) == ':')) {
	    p -= 2;		/* back up over the :: */
	    while ((p >= name) && (*p == ':')) {
		p--;		/* back up over the preceeding : */
	    }
	    break;
        }
    }

    if (p >= name) {
        length = p-name+1;
        Tcl_AppendToObj(Tcl_GetObjResult(interp), name, length);
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * NamespaceTailCmd --
 *
 *	Invoked to implement the "namespace tail" command that returns the
 *	trailing name at the end of a string with "::" namespace
 *	qualifiers. These qualifiers are namespace names separated by
 *	"::"s. For example, for "::foo::p" this command returns "p", and for
 *	"::" it returns "". This command is the complement of the "namespace
 *	qualifiers" command. Note that this command does not check whether
 *	the "namespace" names are, in fact, the names of currently defined
 *	namespaces. Handles the following syntax:
 *
 *	    namespace tail string
 *
 * Results:
 *	Returns TCL_OK if successful, and TCL_ERROR if anything goes wrong.
 *
 * Side effects:
 *	Returns a result in the interpreter's result object. If anything
 *	goes wrong, the result is an error message.
 *
 *----------------------------------------------------------------------
 */

static int
NamespaceTailCmd(dummy, interp, objc, objv)
    ClientData dummy;		/* Not used. */
    Tcl_Interp *interp;		/* Current interpreter. */
    int objc;			/* Number of arguments. */
    Tcl_Obj *CONST objv[];	/* Argument objects. */
{
    register char *name, *p;

    if (objc != 3) {
	Tcl_WrongNumArgs(interp, 2, objv, "string");
        return TCL_ERROR;
    }

    /*
     * Find the end of the string, then work backward and find the
     * last "::" qualifier.
     */

    name = Tcl_GetString(objv[2]);
    for (p = name;  *p != '\0';  p++) {
	/* empty body */
    }
    while (--p > name) {
        if ((*p == ':') && (*(p-1) == ':')) {
            p++;		/* just after the last "::" */
            break;
        }
    }
    
    if (p >= name) {
        Tcl_AppendToObj(Tcl_GetObjResult(interp), p, -1);
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * NamespaceWhichCmd --
 *
 *	Invoked to implement the "namespace which" command that returns the
 *	fully-qualified name of a command or variable. If the specified
 *	command or variable does not exist, it returns "". Handles the
 *	following syntax:
 *
 *	    namespace which ?-command? ?-variable? name
 *
 * Results:
 *	Returns TCL_OK if successful, and TCL_ERROR if anything goes wrong.
 *
 * Side effects:
 *	Returns a result in the interpreter's result object. If anything
 *	goes wrong, the result is an error message.
 *
 *----------------------------------------------------------------------
 */

static int
NamespaceWhichCmd(dummy, interp, objc, objv)
    ClientData dummy;                   /* Not used. */
    Tcl_Interp *interp;                 /* Current interpreter. */
    int objc;                           /* Number of arguments. */
    Tcl_Obj *CONST objv[];              /* Argument objects. */
{
    register char *arg;
    Tcl_Command cmd;
    Tcl_Var variable;
    int argIndex, lookup;

    if (objc < 3) {
        badArgs:
        Tcl_WrongNumArgs(interp, 2, objv,
	        "?-command? ?-variable? name");
        return TCL_ERROR;
    }

    /*
     * Look for a flag controlling the lookup.
     */

    argIndex = 2;
    lookup = 0;			/* assume command lookup by default */
    arg = Tcl_GetString(objv[2]);
    if (*arg == '-') {
	if (strncmp(arg, "-command", 8) == 0) {
	    lookup = 0;
	} else if (strncmp(arg, "-variable", 9) == 0) {
	    lookup = 1;
	} else {
	    goto badArgs;
	}
	argIndex = 3;
    }
    if (objc != (argIndex + 1)) {
	goto badArgs;
    }

    switch (lookup) {
    case 0:			/* -command */
	cmd = Tcl_GetCommandFromObj(interp, objv[argIndex]);
        if (cmd == (Tcl_Command) NULL) {	
            return TCL_OK;	/* cmd not found, just return (no error) */
        }
	Tcl_GetCommandFullName(interp, cmd, Tcl_GetObjResult(interp));
        break;

    case 1:			/* -variable */
        arg = Tcl_GetString(objv[argIndex]);
	variable = Tcl_FindNamespaceVar(interp, arg, (Tcl_Namespace *) NULL,
		/*flags*/ 0);
        if (variable != (Tcl_Var) NULL) {
            Tcl_GetVariableFullName(interp, variable, Tcl_GetObjResult(interp));
        }
        break;
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * FreeNsNameInternalRep --
 *
 *	Frees the resources associated with a nsName object's internal
 *	representation.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Decrements the ref count of any Namespace structure pointed
 *	to by the nsName's internal representation. If there are no more
 *	references to the namespace, it's structure will be freed.
 *
 *----------------------------------------------------------------------
 */

static void
FreeNsNameInternalRep(objPtr)
    register Tcl_Obj *objPtr;   /* nsName object with internal
                                 * representation to free */
{
    register ResolvedNsName *resNamePtr =
        (ResolvedNsName *) objPtr->internalRep.otherValuePtr;
    Namespace *nsPtr;

    /*
     * Decrement the reference count of the namespace. If there are no
     * more references, free it up.
     */

    if (resNamePtr != NULL) {
        resNamePtr->refCount--;
        if (resNamePtr->refCount == 0) {

            /*
	     * Decrement the reference count for the cached namespace.  If
	     * the namespace is dead, and there are no more references to
	     * it, free it.
	     */

            nsPtr = resNamePtr->nsPtr;
            nsPtr->refCount--;
            if ((nsPtr->refCount == 0) && (nsPtr->flags & NS_DEAD)) {
                NamespaceFree(nsPtr);
            }
            ckfree((char *) resNamePtr);
        }
    }
}

/*
 *----------------------------------------------------------------------
 *
 * DupNsNameInternalRep --
 *
 *	Initializes the internal representation of a nsName object to a copy
 *	of the internal representation of another nsName object.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	copyPtr's internal rep is set to refer to the same namespace
 *	referenced by srcPtr's internal rep. Increments the ref count of
 *	the ResolvedNsName structure used to hold the namespace reference.
 *
 *----------------------------------------------------------------------
 */

static void
DupNsNameInternalRep(srcPtr, copyPtr)
    Tcl_Obj *srcPtr;                /* Object with internal rep to copy. */
    register Tcl_Obj *copyPtr;      /* Object with internal rep to set. */
{
    register ResolvedNsName *resNamePtr =
        (ResolvedNsName *) srcPtr->internalRep.otherValuePtr;

    copyPtr->internalRep.otherValuePtr = (VOID *) resNamePtr;
    if (resNamePtr != NULL) {
        resNamePtr->refCount++;
    }
    copyPtr->typePtr = &tclNsNameType;
}

/*
 *----------------------------------------------------------------------
 *
 * SetNsNameFromAny --
 *
 *	Attempt to generate a nsName internal representation for a
 *	Tcl object.
 *
 * Results:
 *	Returns TCL_OK if the value could be converted to a proper
 *	namespace reference. Otherwise, it returns TCL_ERROR, along
 *	with an error message in the interpreter's result object.
 *
 * Side effects:
 *	If successful, the object is made a nsName object. Its internal rep
 *	is set to point to a ResolvedNsName, which contains a cached pointer
 *	to the Namespace. Reference counts are kept on both the
 *	ResolvedNsName and the Namespace, so we can keep track of their
 *	usage and free them when appropriate.
 *
 *----------------------------------------------------------------------
 */

static int
SetNsNameFromAny(interp, objPtr)
    Tcl_Interp *interp;		/* Points to the namespace in which to
				 * resolve name. Also used for error
				 * reporting if not NULL. */
    register Tcl_Obj *objPtr;	/* The object to convert. */
{
    register Tcl_ObjType *oldTypePtr = objPtr->typePtr;
    char *name;
    CONST char *dummy;
    Namespace *nsPtr, *dummy1Ptr, *dummy2Ptr;
    register ResolvedNsName *resNamePtr;

    /*
     * Get the string representation. Make it up-to-date if necessary.
     */

    name = objPtr->bytes;
    if (name == NULL) {
	name = Tcl_GetString(objPtr);
    }

    /*
     * Look for the namespace "name" in the current namespace. If there is
     * an error parsing the (possibly qualified) name, return an error.
     * If the namespace isn't found, we convert the object to an nsName
     * object with a NULL ResolvedNsName* internal rep.
     */

    TclGetNamespaceForQualName(interp, name, (Namespace *) NULL,
            FIND_ONLY_NS, &nsPtr, &dummy1Ptr, &dummy2Ptr, &dummy);

    /*
     * If we found a namespace, then create a new ResolvedNsName structure
     * that holds a reference to it.
     */

    if (nsPtr != NULL) {
	Namespace *currNsPtr =
	        (Namespace *) Tcl_GetCurrentNamespace(interp);
	
        nsPtr->refCount++;
        resNamePtr = (ResolvedNsName *) ckalloc(sizeof(ResolvedNsName));
        resNamePtr->nsPtr = nsPtr;
        resNamePtr->nsId = nsPtr->nsId;
        resNamePtr->refNsPtr = currNsPtr;
        resNamePtr->refCount = 1;
    } else {
        resNamePtr = NULL;
    }

    /*
     * Free the old internalRep before setting the new one.
     * We do this as late as possible to allow the conversion code
     * (in particular, Tcl_GetStringFromObj) to use that old internalRep.
     */

    if ((oldTypePtr != NULL) && (oldTypePtr->freeIntRepProc != NULL)) {
        oldTypePtr->freeIntRepProc(objPtr);
    }

    objPtr->internalRep.otherValuePtr = (VOID *) resNamePtr;
    objPtr->typePtr = &tclNsNameType;
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * UpdateStringOfNsName --
 *
 *	Updates the string representation for a nsName object.
 *	Note: This procedure does not free an existing old string rep
 *	so storage will be lost if this has not already been done.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The object's string is set to a copy of the fully qualified
 *	namespace name.
 *
 *----------------------------------------------------------------------
 */

static void
UpdateStringOfNsName(objPtr)
    register Tcl_Obj *objPtr; /* nsName object with string rep to update. */
{
    ResolvedNsName *resNamePtr =
        (ResolvedNsName *) objPtr->internalRep.otherValuePtr;
    register Namespace *nsPtr;
    char *name = "";
    int length;

    if ((resNamePtr != NULL)
	    && (resNamePtr->nsId == resNamePtr->nsPtr->nsId)) {
        nsPtr = resNamePtr->nsPtr;
        if (nsPtr->flags & NS_DEAD) {
            nsPtr = NULL;
        }
        if (nsPtr != NULL) {
            name = nsPtr->fullName;
        }
    }

    /*
     * The following sets the string rep to an empty string on the heap
     * if the internal rep is NULL.
     */

    length = strlen(name);
    if (length == 0) {
	objPtr->bytes = tclEmptyStringRep;
    } else {
	objPtr->bytes = (char *) ckalloc((unsigned) (length + 1));
	memcpy((VOID *) objPtr->bytes, (VOID *) name, (unsigned) length);
	objPtr->bytes[length] = '\0';
    }
    objPtr->length = length;
}

/*
 *----------------------------------------------------------------------
 *
 * NamespaceEnsembleCmd --
 *
 *	Invoked to implement the "namespace ensemble" command that
 *	creates and manipulates ensembles built on top of namespaces.
 *	Handles the following syntax:
 *
 *	    namespace ensemble name ?dictionary?
 *
 * Results:
 *	Returns TCL_OK if successful, and TCL_ERROR if anything goes wrong.
 *
 * Side effects:
 *	Creates the ensemble for the namespace if one did not
 *	previously exist.  Alternatively, alters the way that the
 *	ensemble's subcommand => implementation prefix is configured.
 *
 *----------------------------------------------------------------------
 */

static int
NamespaceEnsembleCmd(dummy, interp, objc, objv)
    ClientData dummy;
    Tcl_Interp *interp;
    int objc;
    Tcl_Obj *CONST objv[];
{
    Namespace *nsPtr;
    EnsembleConfig *ensemblePtr;
    static CONST char *subcommands[] = {
	"configure", "create", "exists", NULL
    };
    enum EnsSubcmds {
	ENS_CONFIG, ENS_CREATE, ENS_EXISTS
    };
    static CONST char *createOptions[] = {
	"-command", "-map", "-prefixes", "-subcommands", "-unknown", NULL
    };
    enum EnsCreateOpts {
	CRT_CMD, CRT_MAP, CRT_PREFIX, CRT_SUBCMDS, CRT_UNKNOWN
    };
    static CONST char *configOptions[] = {
	"-map", "-namespace", "-prefixes", "-subcommands", "-unknown", NULL
    };
    enum EnsConfigOpts {
	CONF_MAP, CONF_NAMESPACE, CONF_PREFIX, CONF_SUBCMDS, CONF_UNKNOWN
    };
    int index;

    nsPtr = (Namespace *) Tcl_GetCurrentNamespace(interp);
    if (nsPtr == NULL || nsPtr->flags & NS_DEAD) {
	if (!Tcl_InterpDeleted(interp)) {
	    Tcl_AppendResult(interp,
		    "tried to manipulate ensemble of deleted namespace", NULL);
	}
	return TCL_ERROR;
    }

    if (objc < 3) {
	Tcl_WrongNumArgs(interp, 2, objv, "subcommand ?arg ...?");
	return TCL_ERROR;
    }
    if (Tcl_GetIndexFromObj(interp, objv[2], subcommands, "subcommand", 0,
	    &index) != TCL_OK) {
	return TCL_ERROR;
    }

    switch ((enum EnsSubcmds) index) {
    case ENS_CREATE: {
	char *name;
	Tcl_DictSearch search;
	Tcl_Obj *listObj, *nameObj = NULL;
	int done, len, allocatedMapFlag = 0;
	/*
	 * Defaults
	 */
	Tcl_Obj *subcmdObj = NULL;
	Tcl_Obj *mapObj = NULL;
	int permitPrefix = 1;
	Tcl_Obj *unknownObj = NULL;

	objv += 3;
	objc -= 3;

	/*
	 * Work out what name to use for the command to create.  If
	 * supplied, it is either fully specified or relative to the
	 * current namespace.  If not supplied, it is exactly the name
	 * of the current namespace.
	 */

	name = nsPtr->fullName;

	/*
	 * Parse the option list, applying type checks as we go.  Note
	 * that we are not incrementing any reference counts in the
	 * objects at this stage, so the presence of an option
	 * multiple times won't cause any memory leaks.
	 */

	for (; objc>1 ; objc-=2,objv+=2 ) {
	    if (Tcl_GetIndexFromObj(interp, objv[0], createOptions, "option",
		    0, &index) != TCL_OK) {
		if (allocatedMapFlag) {
		    Tcl_DecrRefCount(mapObj);
		}
		return TCL_ERROR;
	    }
	    switch ((enum EnsCreateOpts) index) {
	    case CRT_CMD:
		name = TclGetString(objv[1]);
		continue;
	    case CRT_SUBCMDS:
		if (Tcl_ListObjLength(interp, objv[1], &len) != TCL_OK) {
		    if (allocatedMapFlag) {
			Tcl_DecrRefCount(mapObj);
		    }
		    return TCL_ERROR;
		}
		subcmdObj = (len > 0 ? objv[1] : NULL);
		continue;
	    case CRT_MAP: {
		Tcl_Obj *patchedDict = NULL, *subcmdObj;
		/*
		 * Verify that the map is sensible.
		 */
		if (Tcl_DictObjFirst(interp, objv[1], &search,
			&subcmdObj, &listObj, &done) != TCL_OK) {
		    if (allocatedMapFlag) {
			Tcl_DecrRefCount(mapObj);
		    }
		    return TCL_ERROR;
		}
		if (done) {
		    mapObj = NULL;
		    continue;
		}
		do {
		    Tcl_Obj **listv;
		    char *cmd;

		    if (Tcl_ListObjGetElements(interp, listObj, &len,
			    &listv) != TCL_OK) {
			Tcl_DictObjDone(&search);
			if (patchedDict) {
			    Tcl_DecrRefCount(patchedDict);
			}
			if (allocatedMapFlag) {
			    Tcl_DecrRefCount(mapObj);
			}
			return TCL_ERROR;
		    }
		    if (len < 1) {
			Tcl_SetResult(interp,
				"ensemble subcommand implementations "
				"must be non-empty lists", TCL_STATIC);
			Tcl_DictObjDone(&search);
			if (patchedDict) {
			    Tcl_DecrRefCount(patchedDict);
			}
			if (allocatedMapFlag) {
			    Tcl_DecrRefCount(mapObj);
			}
			return TCL_ERROR;
		    }
		    cmd = TclGetString(listv[0]);
		    if (!(cmd[0] == ':' && cmd[1] == ':')) {
			Tcl_Obj *newList = Tcl_NewListObj(len, listv);
			Tcl_Obj *newCmd =
				Tcl_NewStringObj(nsPtr->fullName, -1);
			if (nsPtr->parentPtr) {
			    Tcl_AppendStringsToObj(newCmd, "::", NULL);
			}
			Tcl_AppendObjToObj(newCmd, listv[0]);
			Tcl_ListObjReplace(NULL, newList, 0, 1, 1, &newCmd);
			if (patchedDict == NULL) {
			    patchedDict = Tcl_DuplicateObj(objv[1]);
			}
			Tcl_DictObjPut(NULL, patchedDict, subcmdObj, newList);
		    }
		    Tcl_DictObjNext(&search, &subcmdObj, &listObj, &done);
		} while (!done);
		if (allocatedMapFlag) {
		    Tcl_DecrRefCount(mapObj);
		}
		mapObj = (patchedDict ? patchedDict : objv[1]);
		if (patchedDict) {
		    allocatedMapFlag = 1;
		}
		continue;
	    }
	    case CRT_PREFIX:
		if (Tcl_GetBooleanFromObj(interp, objv[1],
			&permitPrefix) != TCL_OK) {
		    if (allocatedMapFlag) {
			Tcl_DecrRefCount(mapObj);
		    }
		    return TCL_ERROR;
		}
		continue;
	    case CRT_UNKNOWN:
		if (Tcl_ListObjLength(interp, objv[1], &len) != TCL_OK) {
		    if (allocatedMapFlag) {
			Tcl_DecrRefCount(mapObj);
		    }
		    return TCL_ERROR;
		}
		unknownObj = (len > 0 ? objv[1] : NULL);
		continue;
	    }
	}

	/*
	 * Make the name of the ensemble into a fully qualified name.
	 * This might allocate an object.
	 */

	if (!(name[0] == ':' && name[1] == ':')) {
	    nameObj = Tcl_NewStringObj(nsPtr->fullName, -1);
	    if (nsPtr->parentPtr == NULL) {
		Tcl_AppendStringsToObj(nameObj, name, NULL);
	    } else {
		Tcl_AppendStringsToObj(nameObj, "::", name, NULL);
	    }
	    Tcl_IncrRefCount(nameObj);
	    name = TclGetString(nameObj);
	}

	/*
	 * Create the ensemble.  Note that this might delete another
	 * ensemble linked to the same namespace, so we must be
	 * careful.  However, we should be OK because we only link the
	 * namespace into the list once we've created it (and after
	 * any deletions have occurred.)
	 */

	ensemblePtr = (EnsembleConfig *) ckalloc(sizeof(EnsembleConfig));
	ensemblePtr->nsPtr = nsPtr;
	ensemblePtr->epoch = 0;
	Tcl_InitHashTable(&ensemblePtr->subcommandTable, TCL_STRING_KEYS);
	ensemblePtr->subcommandArrayPtr = NULL;
	ensemblePtr->subcmdList = subcmdObj;
	if (subcmdObj != NULL) {
	    Tcl_IncrRefCount(subcmdObj);
	}
	ensemblePtr->subcommandDict = mapObj;
	if (mapObj != NULL) {
	    Tcl_IncrRefCount(mapObj);
	}
	ensemblePtr->flags = (permitPrefix ? ENS_PREFIX : 0);
	ensemblePtr->unknownHandler = unknownObj;
	if (unknownObj != NULL) {
	    Tcl_IncrRefCount(unknownObj);
	}
	ensemblePtr->token = Tcl_CreateObjCommand(interp, name,
		NsEnsembleImplementationCmd, (ClientData)ensemblePtr,
		DeleteEnsembleConfig);
	ensemblePtr->next = (EnsembleConfig *) nsPtr->ensembles;
	nsPtr->ensembles = (Tcl_Ensemble *) ensemblePtr;
	/*
	 * Trigger an eventual recomputation of the ensemble command
	 * set.  Note that this is slightly tricky, as it means that
	 * we are not actually counting the number of namespace export
	 * actions, but it is the simplest way to go!
	 */
	nsPtr->exportLookupEpoch++;
	Tcl_SetResult(interp, name, TCL_VOLATILE);
	if (nameObj != NULL) {
	    Tcl_DecrRefCount(nameObj);
	}
	return TCL_OK;
    }

    case ENS_EXISTS: {
	Command *cmdPtr;
	int flag;

	if (objc != 4) {
	    Tcl_WrongNumArgs(interp, 3, objv, "cmdname");
	    return TCL_ERROR;
	}
	cmdPtr = (Command *)
		Tcl_FindCommand(interp, TclGetString(objv[3]), 0, 0);
	flag = (cmdPtr != NULL &&
		cmdPtr->objProc == NsEnsembleImplementationCmd);
	Tcl_SetBooleanObj(Tcl_GetObjResult(interp), flag);
	return TCL_OK;
    }

    case ENS_CONFIG: {
	char *cmdName;
	Command *cmdPtr;

	if (objc < 4 || (objc != 5 && objc & 1)) {
	    Tcl_WrongNumArgs(interp, 3, objv, "cmdname ?opt? ?value? ...");
	    return TCL_ERROR;
	}
	cmdName = TclGetString(objv[3]);
	cmdPtr = (Command *)
		Tcl_FindCommand(interp, cmdName, 0, TCL_LEAVE_ERR_MSG);
	if (cmdPtr == NULL) {
	    return TCL_ERROR;
	}
	if (cmdPtr->objProc != NsEnsembleImplementationCmd) {
            Tcl_AppendResult(interp, cmdName, " is not an ensemble command",
		    NULL);
            return TCL_ERROR;
        }
	ensemblePtr = (EnsembleConfig *) cmdPtr->objClientData;

	if (objc == 5) {
	    if (Tcl_GetIndexFromObj(interp, objv[4], configOptions, "option",
		    0, &index) != TCL_OK) {
		return TCL_ERROR;
	    }
	    switch ((enum EnsConfigOpts) index) {
	    case CONF_SUBCMDS:
		if (ensemblePtr->subcmdList != NULL) {
		    Tcl_SetObjResult(interp, ensemblePtr->subcmdList);
		}
		break;
	    case CONF_MAP:
		if (ensemblePtr->subcommandDict != NULL) {
		    Tcl_SetObjResult(interp, ensemblePtr->subcommandDict);
		}
		break;
	    case CONF_NAMESPACE:
		Tcl_SetResult(interp, ensemblePtr->nsPtr->fullName,
			TCL_VOLATILE);
		break;
	    case CONF_PREFIX:
		Tcl_SetObjResult(interp,
			Tcl_NewBooleanObj(ensemblePtr->flags & ENS_PREFIX));
		break;
	    case CONF_UNKNOWN:
		if (ensemblePtr->unknownHandler != NULL) {
		    Tcl_SetObjResult(interp, ensemblePtr->unknownHandler);
		}
		break;
	    }
	    return TCL_OK;

	} else if (objc == 4) {
	    /*
	     * Produce list of all information.
	     */

	    Tcl_Obj *resultObj;

	    TclNewObj(resultObj);
	    Tcl_ListObjAppendElement(NULL, resultObj,
		    Tcl_NewStringObj(configOptions[CONF_MAP], -1));
	    if (ensemblePtr->subcommandDict != NULL) {
		Tcl_ListObjAppendElement(NULL, resultObj,
			ensemblePtr->subcommandDict);
	    } else {
		Tcl_ListObjAppendElement(NULL, resultObj, Tcl_NewObj());
	    }
	    Tcl_ListObjAppendElement(NULL, resultObj,
		    Tcl_NewStringObj(configOptions[CONF_NAMESPACE], -1));
	    Tcl_ListObjAppendElement(NULL, resultObj,
		    Tcl_NewStringObj(ensemblePtr->nsPtr->fullName, -1));
	    Tcl_ListObjAppendElement(NULL, resultObj,
		    Tcl_NewStringObj(configOptions[CONF_PREFIX], -1));
	    Tcl_ListObjAppendElement(NULL, resultObj,
		    Tcl_NewBooleanObj(ensemblePtr->flags & ENS_PREFIX));
	    Tcl_ListObjAppendElement(NULL, resultObj,
		    Tcl_NewStringObj(configOptions[CONF_SUBCMDS], -1));
	    if (ensemblePtr->subcmdList != NULL) {
		Tcl_ListObjAppendElement(NULL, resultObj,
			ensemblePtr->subcmdList);
	    } else {
		Tcl_ListObjAppendElement(NULL, resultObj, Tcl_NewObj());
	    }
	    Tcl_ListObjAppendElement(NULL, resultObj,
		    Tcl_NewStringObj(configOptions[CONF_UNKNOWN], -1));
	    if (ensemblePtr->unknownHandler != NULL) {
		Tcl_ListObjAppendElement(NULL, resultObj,
			ensemblePtr->unknownHandler);
	    } else {
		Tcl_ListObjAppendElement(NULL, resultObj, Tcl_NewObj());
	    }
	    Tcl_SetObjResult(interp, resultObj);
	    return TCL_OK;

	} else {
	    Tcl_DictSearch search;
	    Tcl_Obj *listObj;
	    int done, len, allocatedMapFlag = 0;
	    /*
	     * Defaults
	     */
	    Tcl_Obj *subcmdObj = ensemblePtr->subcmdList;
	    Tcl_Obj *mapObj = ensemblePtr->subcommandDict;
	    Tcl_Obj *unknownObj = ensemblePtr->unknownHandler;
	    int permitPrefix = ensemblePtr->flags & ENS_PREFIX;

	    objv += 4;
	    objc -= 4;

	    /*
	     * Parse the option list, applying type checks as we go.
	     * Note that we are not incrementing any reference counts
	     * in the objects at this stage, so the presence of an
	     * option multiple times won't cause any memory leaks.
	     */

	    for (; objc>0 ; objc-=2,objv+=2 ) {
		if (Tcl_GetIndexFromObj(interp, objv[0], configOptions,
			"option", 0, &index) != TCL_OK) {
		    if (allocatedMapFlag) {
			Tcl_DecrRefCount(mapObj);
		    }
		    return TCL_ERROR;
		}
		switch ((enum EnsConfigOpts) index) {
		case CONF_SUBCMDS:
		    if (Tcl_ListObjLength(interp, objv[1], &len) != TCL_OK) {
			if (allocatedMapFlag) {
			    Tcl_DecrRefCount(mapObj);
			}
			return TCL_ERROR;
		    }
		    subcmdObj = (len > 0 ? objv[1] : NULL);
		    continue;
		case CONF_MAP: {
		    Tcl_Obj *patchedDict = NULL, *subcmdObj;
		    /*
		     * Verify that the map is sensible.
		     */
		    if (Tcl_DictObjFirst(interp, objv[1], &search,
			    &subcmdObj, &listObj, &done) != TCL_OK) {
			if (allocatedMapFlag) {
			    Tcl_DecrRefCount(mapObj);
			}
			return TCL_ERROR;
		    }
		    if (done) {
			mapObj = NULL;
			continue;
		    }
		    do {
			Tcl_Obj **listv;
			char *cmd;

			if (Tcl_ListObjGetElements(interp, listObj, &len,
				&listv) != TCL_OK) {
			    Tcl_DictObjDone(&search);
			    if (patchedDict) {
				Tcl_DecrRefCount(patchedDict);
			    }
			    if (allocatedMapFlag) {
				Tcl_DecrRefCount(mapObj);
			    }
			    return TCL_ERROR;
			}
			if (len < 1) {
			    Tcl_SetResult(interp,
				    "ensemble subcommand implementations "
				    "must be non-empty lists", TCL_STATIC);
			    Tcl_DictObjDone(&search);
			    if (patchedDict) {
				Tcl_DecrRefCount(patchedDict);
			    }
			    if (allocatedMapFlag) {
				Tcl_DecrRefCount(mapObj);
			    }
			    return TCL_ERROR;
			}
			cmd = TclGetString(listv[0]);
			if (!(cmd[0] == ':' && cmd[1] == ':')) {
			    Tcl_Obj *newList = Tcl_NewListObj(len, listv);
			    Tcl_Obj *newCmd =
				    Tcl_NewStringObj(nsPtr->fullName, -1);
			    if (nsPtr->parentPtr) {
				Tcl_AppendStringsToObj(newCmd, "::", NULL);
			    }
			    Tcl_AppendObjToObj(newCmd, listv[0]);
			    Tcl_ListObjReplace(NULL, newList, 0,1, 1,&newCmd);
			    if (patchedDict == NULL) {
				patchedDict = Tcl_DuplicateObj(objv[1]);
			    }
			    Tcl_DictObjPut(NULL, patchedDict, subcmdObj,
				    newList);
			}
			Tcl_DictObjNext(&search, &subcmdObj, &listObj, &done);
		    } while (!done);
		    if (allocatedMapFlag) {
			Tcl_DecrRefCount(mapObj);
		    }
		    mapObj = (patchedDict ? patchedDict : objv[1]);
		    if (patchedDict) {
			allocatedMapFlag = 1;
		    }
		    continue;
		}
		case CONF_NAMESPACE:
		    if (allocatedMapFlag) {
			Tcl_DecrRefCount(mapObj);
		    }
		    Tcl_AppendResult(interp, "option -namespace is read-only",
			    NULL);
		    return TCL_ERROR;
		case CONF_PREFIX:
		    if (Tcl_GetBooleanFromObj(interp, objv[1],
			    &permitPrefix) != TCL_OK) {
			if (allocatedMapFlag) {
			    Tcl_DecrRefCount(mapObj);
			}
			return TCL_ERROR;
		    }
		    continue;
		case CONF_UNKNOWN:
		    if (Tcl_ListObjLength(interp, objv[1], &len) != TCL_OK) {
			if (allocatedMapFlag) {
			    Tcl_DecrRefCount(mapObj);
			}
			return TCL_ERROR;
		    }
		    unknownObj = (len > 0 ? objv[1] : NULL);
		    continue;
		}
	    }

	    /*
	     * Update the namespace now that we've finished the
	     * parsing stage.
	     */

	    if (ensemblePtr->subcmdList != subcmdObj) {
		if (ensemblePtr->subcmdList != NULL) {
		    Tcl_DecrRefCount(ensemblePtr->subcmdList);
		}
		ensemblePtr->subcmdList = subcmdObj;
		if (subcmdObj != NULL) {
		    Tcl_IncrRefCount(subcmdObj);
		}
	    }
	    if (ensemblePtr->subcommandDict != mapObj) {
		if (ensemblePtr->subcommandDict != NULL) {
		    Tcl_DecrRefCount(ensemblePtr->subcommandDict);
		}
		ensemblePtr->subcommandDict = mapObj;
		if (mapObj != NULL) {
		    Tcl_IncrRefCount(mapObj);
		}
	    }
	    if (ensemblePtr->unknownHandler != unknownObj) {
		if (ensemblePtr->unknownHandler != NULL) {
		    Tcl_DecrRefCount(ensemblePtr->unknownHandler);
		}
		ensemblePtr->unknownHandler = unknownObj;
		if (unknownObj != NULL) {
		    Tcl_IncrRefCount(unknownObj);
		}
	    }
	    if (permitPrefix) {
		ensemblePtr->flags |= ENS_PREFIX;
	    } else {
		ensemblePtr->flags &= ~ENS_PREFIX;
	    }
	    /*
	     * Trigger an eventual recomputation of the ensemble
	     * command set.  Note that this is slightly tricky, as it
	     * means that we are not actually counting the number of
	     * namespace export actions, but it is the simplest way to
	     * go!  Also note that this nsPtr and ensemblePtr->nsPtr
	     * are quite possibly not the same namespace; we want to
	     * bump the epoch for the ensemble's namespace, not the
	     * current namespace.
	     */
	    ensemblePtr->nsPtr->exportLookupEpoch++;
	    return TCL_OK;
	}
    }

    default:
	Tcl_Panic("unexpected ensemble command");
    }
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * NsEnsembleImplementationCmd --
 *
 *	Implements an ensemble of commands (being those exported by a
 *	namespace other than the global namespace) as a command with
 *	the same (short) name as the namespace in the parent namespace.
 *
 * Results:
 *	A standard Tcl result code.  Will be TCL_ERROR if the command
 *	is not an unambiguous prefix of any command exported by the
 *	ensemble's namespace.
 *
 * Side effects:
 *	Depends on the command within the namespace that gets executed.
 *	If the ensemble itself returns TCL_ERROR, a descriptive error
 *	message will be placed in the interpreter's result.
 *
 *----------------------------------------------------------------------
 */

static int
NsEnsembleImplementationCmd(clientData, interp, objc, objv)
    ClientData clientData;
    Tcl_Interp *interp;
    int objc;
    Tcl_Obj *CONST objv[];
{
    EnsembleConfig *ensemblePtr = (EnsembleConfig *) clientData;
					/* The ensemble itself. */
    Tcl_Obj **tempObjv;			/* Space used to construct the list of
					 * arguments to pass to the command
					 * that implements the ensemble
					 * subcommand. */
    int result;				/* The result of the subcommand
					 * execution. */
    Tcl_Obj *prefixObj;			/* An object containing the prefix
					 * words of the command that implements
					 * the subcommand. */
    Tcl_HashEntry *hPtr;		/* Used for efficient lookup of fully
					 * specified but not yet cached command
					 * names. */
    Tcl_Obj **prefixObjv;		/* The list of objects to substitute in
					 * as the target command prefix. */
    int prefixObjc;			/* Size of prefixObjv of course! */
    int reparseCount = 0;		/* Number of reparses. */

    if (objc < 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "subcommand ?argument ...?");
	return TCL_ERROR;
    }

  restartEnsembleParse:
    if (ensemblePtr->nsPtr->flags & NS_DEAD) {
	/*
	 * Don't know how we got here, but make things give up quickly.
	 */
	if (!Tcl_InterpDeleted(interp)) {
	    Tcl_AppendResult(interp,
		    "ensemble activated for deleted namespace", NULL);
	}
	return TCL_ERROR;
    }

    if (ensemblePtr->epoch != ensemblePtr->nsPtr->exportLookupEpoch) {
	ensemblePtr->epoch = ensemblePtr->nsPtr->exportLookupEpoch;
	BuildEnsembleConfig(ensemblePtr);
    } else {
	/*
	 * Table of subcommands is still valid; therefore there might
	 * be a valid cache of discovered information which we can
	 * reuse.  Do the check here, and if we're still valid, we can
	 * jump straight to the part where we do the invocation of the
	 * subcommand.
	 */

	if (objv[1]->typePtr == &tclEnsembleCmdType) {
	    EnsembleCmdRep *ensembleCmd = (EnsembleCmdRep *)
		    objv[1]->internalRep.otherValuePtr;
	    if (ensembleCmd->nsPtr == ensemblePtr->nsPtr &&
		ensembleCmd->epoch == ensemblePtr->epoch) {
		prefixObj = ensembleCmd->realPrefixObj;
		goto runSubcommand;
	    }
	}
    }

    /*
     * Look in the hashtable for the subcommand name; this is the
     * fastest way of all.
     */

    hPtr = Tcl_FindHashEntry(&ensemblePtr->subcommandTable,
	    TclGetString(objv[1]));
    if (hPtr != NULL) {
	char *fullName = Tcl_GetHashKey(&ensemblePtr->subcommandTable, hPtr);
	prefixObj = (Tcl_Obj *) Tcl_GetHashValue(hPtr);

	/*
	 * Cache for later in the subcommand object.
	 */

	MakeCachedEnsembleCommand(objv[1], ensemblePtr, fullName, prefixObj);
    } else if (!(ensemblePtr->flags & ENS_PREFIX)) {
	/*
	 * Can't find and we are prohibited from using unambiguous prefixes.
	 */
	goto unknownOrAmbiguousSubcommand;
    } else {
	/*
	 * If we've not already confirmed the command with the hash as
	 * part of building our export table, we need to scan the
	 * sorted array for matches.
	 */

	char *subcmdName;		/* Name of the subcommand, or unique
					 * prefix of it (will be an error for
					 * a non-unique prefix). */
	char *fullName = NULL;		/* Full name of the subcommand. */
	int stringLength, i;
	int tableLength = ensemblePtr->subcommandTable.numEntries;

	subcmdName = TclGetString(objv[1]);
	stringLength = objv[1]->length;
	for (i=0 ; i<tableLength ; i++) {
	    register int cmp = strncmp(subcmdName,
		    ensemblePtr->subcommandArrayPtr[i],
		    (unsigned)stringLength);
	    if (cmp == 0) {
		if (fullName != NULL) {
		    /*
		     * Since there's never the exact-match case to
		     * worry about (hash search filters this), getting
		     * here indicates that our subcommand is an
		     * ambiguous prefix of (at least) two exported
		     * subcommands, which is an error case.
		     */
		    goto unknownOrAmbiguousSubcommand;
		}
		fullName = ensemblePtr->subcommandArrayPtr[i];
	    } else if (cmp == 1) {
		/*
		 * Because we are searching a sorted table, we can now
		 * stop searching because we have gone past anything
		 * that could possibly match.
		 */
		break;
	    }
	}
	if (fullName == NULL) {
	    /*
	     * The subcommand is not a prefix of anything, so bail out!
	     */
	    goto unknownOrAmbiguousSubcommand;
	}
	hPtr = Tcl_FindHashEntry(&ensemblePtr->subcommandTable, fullName);
	if (hPtr == NULL) {
	    Tcl_Panic("full name %s not found in supposedly synchronized hash",
		    fullName);
	}
	prefixObj = (Tcl_Obj *) Tcl_GetHashValue(hPtr);

	/*
	 * Cache for later in the subcommand object.
	 */

	MakeCachedEnsembleCommand(objv[1], ensemblePtr, fullName, prefixObj);
    }

 runSubcommand:
    /*
     * Do the real work of execution of the subcommand by building an
     * array of objects (note that this is potentially not the same
     * length as the number of arguments to this ensemble command),
     * populating it and then feeding it back through the main
     * command-lookup engine.  In theory, we could look up the command
     * in the namespace ourselves, as we already have the namespace in
     * which it is guaranteed to exist, but we don't do that (the
     * cacheing of the command object used should help with that.)
     */

    Tcl_IncrRefCount(prefixObj);
  runResultingSubcommand:
    Tcl_ListObjGetElements(NULL, prefixObj, &prefixObjc, &prefixObjv);
    tempObjv = (Tcl_Obj **) ckalloc(sizeof(Tcl_Obj *)*(objc-2+prefixObjc));
    memcpy(tempObjv,            prefixObjv, sizeof(Tcl_Obj *) * prefixObjc);
    memcpy(tempObjv+prefixObjc, objv+2,     sizeof(Tcl_Obj *) * (objc-2));
    result = Tcl_EvalObjv(interp, objc-2+prefixObjc, tempObjv, 0);
    Tcl_DecrRefCount(prefixObj);
    ckfree((char *)tempObjv);
    return result;

 unknownOrAmbiguousSubcommand:
    /*
     * Have not been able to match the subcommand asked for with a
     * real subcommand that we export.  See whether a handler has been
     * registered for dealing with this situation.  Will only call (at
     * most) once for any particular ensemble invocation.
     */

    if (ensemblePtr->unknownHandler != NULL && reparseCount++ < 1) {
	int paramc, i;
	Tcl_Obj **paramv, *unknownCmd;
	char *ensName = TclGetString(objv[0]);

	unknownCmd = Tcl_DuplicateObj(ensemblePtr->unknownHandler);
	if (ensName[0] == ':') {
	    Tcl_ListObjAppendElement(NULL, unknownCmd, objv[0]);
	} else {
	    Tcl_Obj *qualEnsembleObj =
		Tcl_NewStringObj(Tcl_GetCurrentNamespace(interp)->fullName,-1);
	    if (Tcl_GetCurrentNamespace(interp)->parentPtr) {
		Tcl_AppendStringsToObj(qualEnsembleObj, "::", ensName, NULL);
	    } else {
		Tcl_AppendStringsToObj(qualEnsembleObj, ensName, NULL);
	    }
	    Tcl_ListObjAppendElement(NULL, unknownCmd, qualEnsembleObj);
	}
	for (i=1 ; i<objc ; i++) {
	    Tcl_ListObjAppendElement(NULL, unknownCmd, objv[i]);
	}
	Tcl_ListObjGetElements(NULL, unknownCmd, &paramc, &paramv);
	Tcl_Preserve(ensemblePtr);
	Tcl_IncrRefCount(unknownCmd);
	result = Tcl_EvalObjv(interp, paramc, paramv, 0);
	if (result == TCL_OK) {
	    prefixObj = Tcl_GetObjResult(interp);
	    Tcl_IncrRefCount(prefixObj);
	    Tcl_DecrRefCount(unknownCmd);
	    Tcl_Release(ensemblePtr);
	    Tcl_ResetResult(interp);
	    if (ensemblePtr->flags & ENS_DEAD) {
		Tcl_DecrRefCount(prefixObj);
		Tcl_SetResult(interp,
			"unknown subcommand handler deleted its ensemble",
			TCL_STATIC);
		return TCL_ERROR;
	    }

	    /*
	     * Namespace is still there.  Check if the result is a
	     * valid list.  If it is, and it is non-empty, that list
	     * is what we are using as our replacement.
	     */

	    if (Tcl_ListObjLength(interp, prefixObj, &prefixObjc) != TCL_OK) {
		Tcl_DecrRefCount(prefixObj);
		Tcl_AddErrorInfo(interp,
		    "\n    while parsing result of ensemble unknown subcommand handler");
		return TCL_ERROR;
	    }
	    if (prefixObjc > 0) {
		/*
		 * Not 'runSubcommand' because we want to get the
		 * object refcounting right.
		 */
		goto runResultingSubcommand;
	    }

	    /*
	     * Namespace alive & empty result => reparse.
	     */

	    goto restartEnsembleParse;
	}
	if (!Tcl_InterpDeleted(interp)) {
	    if (result != TCL_ERROR) {
		Tcl_ResetResult(interp);
		Tcl_SetResult(interp,
			"unknown subcommand handler returned bad code: ",
			TCL_STATIC);
		switch (result) {
		case TCL_RETURN:
		    Tcl_AppendResult(interp, "return", NULL);
		    break;
		case TCL_BREAK:
		    Tcl_AppendResult(interp, "break", NULL);
		    break;
		case TCL_CONTINUE:
		    Tcl_AppendResult(interp, "continue", NULL);
		    break;
		default: {
		    char buf[TCL_INTEGER_SPACE];
		    sprintf(buf, "%d", result);
		    Tcl_AppendResult(interp, buf, NULL);
		}
		}
		Tcl_AddErrorInfo(interp,
		    "\n    result of ensemble unknown subcommand handler: ");
		Tcl_AddErrorInfo(interp, TclGetString(unknownCmd));
	    } else {
		Tcl_AddErrorInfo(interp,
			"\n    (ensemble unknown subcommand handler)");
	    }
	}
	Tcl_DecrRefCount(unknownCmd);
	Tcl_Release(ensemblePtr);
	return TCL_ERROR;
    }
    /*
     * Cannot determine what subcommand to hand off to, so generate a
     * (standard) failure message.  Note the one odd case compared
     * with standard ensemble-like command, which is where a namespace
     * has no exported commands at all...
     */
    Tcl_ResetResult(interp);
    if (ensemblePtr->subcommandTable.numEntries == 0) {
	Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
		"unknown subcommand \"", TclGetString(objv[1]),
		"\": namespace ", ensemblePtr->nsPtr->fullName,
		" does not export any commands", NULL);
	return TCL_ERROR;
    }
    Tcl_AppendStringsToObj(Tcl_GetObjResult(interp), "unknown ",
	    (ensemblePtr->flags & ENS_PREFIX ? "or ambiguous " : ""),
	    "subcommand \"", TclGetString(objv[1]), "\": must be ", NULL);
    if (ensemblePtr->subcommandTable.numEntries == 1) {
	Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
		ensemblePtr->subcommandArrayPtr[0], NULL);
    } else {
	int i;
	for (i=0 ; i<ensemblePtr->subcommandTable.numEntries-1 ; i++) {
	    Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
		    ensemblePtr->subcommandArrayPtr[i], ", ", NULL);
	}
	Tcl_AppendStringsToObj(Tcl_GetObjResult(interp),
		"or ", ensemblePtr->subcommandArrayPtr[i], NULL);
    }
    return TCL_ERROR;
}

/*
 *----------------------------------------------------------------------
 *
 * MakeCachedEnsembleCommand --
 *
 *	Cache what we've computed so far; it's not nice to repeatedly
 *	copy strings about.  Note that to do this, we start by
 *	deleting any old representation that there was (though if it
 *	was an out of date ensemble rep, we can skip some of the
 *	deallocation process.)
 *
 * Results:
 *	None
 *
 * Side effects:
 *	Alters the internal representation of the first object parameter.
 *
 *----------------------------------------------------------------------
 */
static void
MakeCachedEnsembleCommand(objPtr, ensemblePtr, subcommandName, prefixObjPtr)
    Tcl_Obj *objPtr;
    EnsembleConfig *ensemblePtr;
    CONST char *subcommandName;
    Tcl_Obj *prefixObjPtr;
{
    register EnsembleCmdRep *ensembleCmd;
    int length;

    if (objPtr->typePtr == &tclEnsembleCmdType) {
	ensembleCmd = (EnsembleCmdRep *) objPtr->internalRep.otherValuePtr;
	Tcl_DecrRefCount(ensembleCmd->realPrefixObj);
	ensembleCmd->nsPtr->refCount--;
	if ((ensembleCmd->nsPtr->refCount == 0)
		&& (ensembleCmd->nsPtr->flags & NS_DEAD)) {
	    NamespaceFree(ensembleCmd->nsPtr);
	}
	ckfree(ensembleCmd->fullSubcmdName);
    } else {
	/*
	 * Kill the old internal rep, and replace it with a brand new
	 * one of our own.
	 */
	if ((objPtr->typePtr != NULL)
		&& (objPtr->typePtr->freeIntRepProc != NULL)) {
	    objPtr->typePtr->freeIntRepProc(objPtr);
	}
	ensembleCmd = (EnsembleCmdRep *) ckalloc(sizeof(EnsembleCmdRep));
	objPtr->internalRep.otherValuePtr = (VOID *) ensembleCmd;
	objPtr->typePtr = &tclEnsembleCmdType;
    }

    /*
     * Populate the internal rep.
     */
    ensembleCmd->nsPtr = ensemblePtr->nsPtr;
    ensemblePtr->nsPtr->refCount++;
    ensembleCmd->realPrefixObj = prefixObjPtr;
    length = strlen(subcommandName)+1;
    ensembleCmd->fullSubcmdName = ckalloc((unsigned) length);
    memcpy(ensembleCmd->fullSubcmdName, subcommandName, (unsigned) length);
    Tcl_IncrRefCount(ensembleCmd->realPrefixObj);
}

/*
 *----------------------------------------------------------------------
 *
 * DeleteEnsembleConfig --
 *
 *	Destroys the data structure used to represent an ensemble.
 *	This is called when the ensemble's command is deleted (which
 *	happens automatically if the ensemble's namespace is deleted.)
 *	Maintainers should note that ensembles should be deleted by
 *	deleting their commands.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Memory is (eventually) deallocated.
 *
 *----------------------------------------------------------------------
 */

static void
DeleteEnsembleConfig(clientData)
    ClientData clientData;
{
    EnsembleConfig *ensemblePtr = (EnsembleConfig *)clientData;
    Namespace *nsPtr = ensemblePtr->nsPtr;
    Tcl_HashSearch search;
    Tcl_HashEntry *hEnt;

    /*
     * Unlink from the ensemble chain if it has not been marked as
     * having been done already.
     */

    if (ensemblePtr->next != ensemblePtr) {
	EnsembleConfig *ensPtr = (EnsembleConfig *) nsPtr->ensembles;
	if (ensPtr == ensemblePtr) {
	    nsPtr->ensembles = (Tcl_Ensemble *) ensemblePtr->next;
	} else {
	    while (ensPtr != NULL) {
		if (ensPtr->next == ensemblePtr) {
		    ensPtr->next = ensemblePtr->next;
		    break;
		}
		ensPtr = ensPtr->next;
	    }
	}
    }

    /*
     * Mark the namespace as dead so code that uses Tcl_Preserve() can
     * tell whether disaster happened anyway.
     */

    ensemblePtr->flags |= ENS_DEAD;

    /*
     * Kill the pointer-containing fields.
     */

    if (ensemblePtr->subcommandTable.numEntries != 0) {
	ckfree((char *)ensemblePtr->subcommandArrayPtr);
    }
    hEnt = Tcl_FirstHashEntry(&ensemblePtr->subcommandTable, &search);
    while (hEnt != NULL) {
	Tcl_Obj *prefixObj = (Tcl_Obj *) Tcl_GetHashValue(hEnt);
	Tcl_DecrRefCount(prefixObj);
	hEnt = Tcl_NextHashEntry(&search);
    }
    Tcl_DeleteHashTable(&ensemblePtr->subcommandTable);
    if (ensemblePtr->subcmdList != NULL) {
	Tcl_DecrRefCount(ensemblePtr->subcmdList);
    }
    if (ensemblePtr->subcommandDict != NULL) {
	Tcl_DecrRefCount(ensemblePtr->subcommandDict);
    }
    if (ensemblePtr->unknownHandler != NULL) {
	Tcl_DecrRefCount(ensemblePtr->unknownHandler);
    }

    /*
     * Arrange for the structure to be reclaimed.  Note that this is
     * complex because we have to make sure that we can react sensibly
     * when an ensemble is deleted during the process of initialising
     * the ensemble (especially the unknown callback.)
     */

    Tcl_EventuallyFree((ClientData) ensemblePtr, TCL_DYNAMIC);
}

/*
 *----------------------------------------------------------------------
 *
 * BuildEnsembleConfig -- 
 *
 *	Create the internal data structures that describe how an
 *	ensemble looks, being a hash mapping from the full command
 *	name to the Tcl list that describes the implementation prefix
 *	words, and a sorted array of all the full command names to
 *	allow for reasonably efficient unambiguous prefix handling.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Reallocates and rebuilds the hash table and array stored at
 *	the ensemblePtr argument.  For large ensembles or large
 *	namespaces, this is a potentially expensive operation.
 *
 *----------------------------------------------------------------------
 */

static void
BuildEnsembleConfig(ensemblePtr)
    EnsembleConfig *ensemblePtr;
{
    Tcl_HashSearch search;		/* Used for scanning the set of
					 * commands in the namespace that
					 * backs up this ensemble. */
    int i, j, isNew;
    Tcl_HashTable *hash = &ensemblePtr->subcommandTable;
    Tcl_HashEntry *hPtr;

    if (hash->numEntries != 0) {
	/*
	 * Remove pre-existing table.
	 */
	ckfree((char *)ensemblePtr->subcommandArrayPtr);
	Tcl_DeleteHashTable(hash);
	Tcl_InitHashTable(hash, TCL_STRING_KEYS);
    }

    /*
     * See if we've got an export list.  If so, we will only export
     * exactly those commands, which may be either implemented by the
     * prefix in the subcommandDict or mapped directly onto the
     * namespace's commands.
     */

    if (ensemblePtr->subcmdList != NULL) {
	Tcl_Obj **subcmdv, *target, *cmdObj, *cmdPrefixObj;
	int subcmdc;

	Tcl_ListObjGetElements(NULL, ensemblePtr->subcmdList, &subcmdc,
		&subcmdv);
	for (i=0 ; i<subcmdc ; i++) {
	    char *name = TclGetString(subcmdv[i]);

	    hPtr = Tcl_CreateHashEntry(hash, name, &isNew);

	    /* Skip non-unique cases. */
	    if (!isNew) {
		continue;
	    }
	    /*
	     * Look in our dictionary (if present) for the command.
	     */
	    if (ensemblePtr->subcommandDict != NULL) {
		Tcl_DictObjGet(NULL, ensemblePtr->subcommandDict, subcmdv[i],
			&target);
		if (target != NULL) {
		    Tcl_SetHashValue(hPtr, (ClientData) target);
		    Tcl_IncrRefCount(target);
		    continue;
		}
	    }
	    /*
	     * Not there, so map onto the namespace.  Note in this
	     * case that we do not guarantee that the command is
	     * actually there; that is the programmer's responsibility
	     * (or [::unknown] of course).
	     */
	    cmdObj = Tcl_NewStringObj(ensemblePtr->nsPtr->fullName, -1);
	    if (ensemblePtr->nsPtr->parentPtr != NULL) {
		Tcl_AppendStringsToObj(cmdObj, "::", name, NULL);
	    } else {
		Tcl_AppendStringsToObj(cmdObj, name, NULL);
	    }
	    cmdPrefixObj = Tcl_NewListObj(1, &cmdObj);
	    Tcl_SetHashValue(hPtr, (ClientData) cmdPrefixObj);
	    Tcl_IncrRefCount(cmdPrefixObj);
	}
    } else if (ensemblePtr->subcommandDict != NULL) {
	/*
	 * No subcmd list, but we do have a mapping dictionary so we
	 * should use the keys of that.  Convert the dictionary's
	 * contents into the form required for the ensemble's internal
	 * hashtable.
	 */
	Tcl_DictSearch dictSearch;
	Tcl_Obj *keyObj, *valueObj;
	int done;

	Tcl_DictObjFirst(NULL, ensemblePtr->subcommandDict, &dictSearch,
		&keyObj, &valueObj, &done);
	while (!done) {
	    char *name = TclGetString(keyObj);
	    hPtr = Tcl_CreateHashEntry(hash, name, &isNew);
	    Tcl_SetHashValue(hPtr, (ClientData) valueObj);
	    Tcl_IncrRefCount(valueObj);
	    Tcl_DictObjNext(&dictSearch, &keyObj, &valueObj, &done);
	}
    } else {
	/*
	 * Discover what commands are actually exported by the
	 * namespace.  What we have is an array of patterns and a hash
	 * table whose keys are the command names exported by the
	 * namespace (the contents do not matter here.)  We must find
	 * out what commands are actually exported by filtering each
	 * command in the namespace against each of the patterns in
	 * the export list.  Note that we use an intermediate hash
	 * table to make memory management easier, and because that
	 * makes exact matching far easier too.
	 *
	 * Suggestion for future enhancement: compute the unique
	 * prefixes and place them in the hash too, which should make
	 * for even faster matching.
	 */

	hPtr = Tcl_FirstHashEntry(&ensemblePtr->nsPtr->cmdTable, &search);
	for (; hPtr!= NULL ; hPtr=Tcl_NextHashEntry(&search)) {
	    char *nsCmdName =		/* Name of command in namespace. */
		    Tcl_GetHashKey(&ensemblePtr->nsPtr->cmdTable, hPtr);

	    for (i=0 ; i<ensemblePtr->nsPtr->numExportPatterns ; i++) {
		if (Tcl_StringMatch(nsCmdName,
			ensemblePtr->nsPtr->exportArrayPtr[i])) {
		    hPtr = Tcl_CreateHashEntry(hash, nsCmdName, &isNew);

		    /*
		     * Remember, hash entries have a full reference to
		     * the substituted part of the command (as a list)
		     * as their content!
		     */

		    if (isNew) {
			Tcl_Obj *cmdObj, *cmdPrefixObj;

			TclNewObj(cmdObj);
			Tcl_AppendStringsToObj(cmdObj,
				ensemblePtr->nsPtr->fullName,
				(ensemblePtr->nsPtr->parentPtr ? "::" : ""),
				nsCmdName, NULL);
			cmdPrefixObj = Tcl_NewListObj(1, &cmdObj);
			Tcl_SetHashValue(hPtr, (ClientData) cmdPrefixObj);
			Tcl_IncrRefCount(cmdPrefixObj);
		    }
		    break;
		}
	    }
	}
    }

    if (hash->numEntries == 0) {
	ensemblePtr->subcommandArrayPtr = NULL;
	return;
    }

    /*
     * Create a sorted array of all subcommands in the ensemble; hash
     * tables are all very well for a quick look for an exact match,
     * but they can't determine things like whether a string is a
     * prefix of another (not without lots of preparation anyway) and
     * they're no good for when we're generating the error message
     * either.
     *
     * We do this by filling an array with the names (we use the hash
     * keys directly to save a copy, since any time we change the
     * array we change the hash too, and vice versa) and running
     * quicksort over the array.
     */

    ensemblePtr->subcommandArrayPtr = (char **)
	    ckalloc(sizeof(char *) * hash->numEntries);

    /*
     * Fill array from both ends as this makes us less likely to end
     * up with performance problems in qsort(), which is good.  Note
     * that doing this makes this code much more opaque, but the naive
     * alternatve:
     *
     * for (hPtr=Tcl_FirstHashEntry(hash,&search),i=0 ; 
     *	   hPtr!=NULL ; hPtr=Tcl_NextHashEntry(&search),i++) {
     *     ensemblePtr->subcommandArrayPtr[i] =
     *	       Tcl_GetHashKey(hash, &hPtr);
     * }
     *
     * can produce long runs of precisely ordered table entries when
     * the commands in the namespace are declared in a sorted fashion
     * (an ordering some people like) and the hashing functions (or
     * the command names themselves) are fairly unfortunate.  By
     * filling from both ends, it requires active malice (and probably
     * a debugger) to get qsort() to have awful runtime behaviour.
     */

    i = 0;
    j = hash->numEntries;
    hPtr = Tcl_FirstHashEntry(hash, &search);
    while (hPtr != NULL) {
	ensemblePtr->subcommandArrayPtr[i++] = Tcl_GetHashKey(hash, hPtr);
	hPtr = Tcl_NextHashEntry(&search);
	if (hPtr == NULL) {
	    break;
	}
	ensemblePtr->subcommandArrayPtr[--j] = Tcl_GetHashKey(hash, hPtr);
	hPtr = Tcl_NextHashEntry(&search);
    }
    if (hash->numEntries > 1) {
	qsort(ensemblePtr->subcommandArrayPtr, (unsigned)hash->numEntries,
		sizeof(char *), NsEnsembleStringOrder);
    }
}

/*
 *----------------------------------------------------------------------
 *
 * NsEnsembleStringOrder --
 *
 *      Helper function to compare two pointers to two strings for use
 *      with qsort().
 *
 * Results:
 *      -1 if the first string is smaller, 1 if the second string is
 *      smaller, and 0 if they are equal.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static int
NsEnsembleStringOrder(strPtr1, strPtr2)
    CONST VOID *strPtr1, *strPtr2;
{
    return strcmp(*(CONST char **)strPtr1, *(CONST char **)strPtr2);
}

/*
 *----------------------------------------------------------------------
 *
 * FreeEnsembleCmdRep --
 *
 *	Destroys the internal representation of a Tcl_Obj that has been
 *	holding information about a command in an ensemble.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Memory is deallocated.  If this held the last reference to a
 *	namespace's main structure, that main structure will also be
 *	destroyed.
 *
 *----------------------------------------------------------------------
 */

static void
FreeEnsembleCmdRep(objPtr)
    Tcl_Obj *objPtr;
{
    EnsembleCmdRep *ensembleCmd = (EnsembleCmdRep *)
	    objPtr->internalRep.otherValuePtr;

    Tcl_DecrRefCount(ensembleCmd->realPrefixObj);
    ckfree(ensembleCmd->fullSubcmdName);
    ensembleCmd->nsPtr->refCount--;
    if ((ensembleCmd->nsPtr->refCount == 0)
	    && (ensembleCmd->nsPtr->flags & NS_DEAD)) {
	NamespaceFree(ensembleCmd->nsPtr);
    }
    ckfree((char *)ensembleCmd);
}

/*
 *----------------------------------------------------------------------
 *
 * DupEnsembleCmdRep --
 *
 *	Makes one Tcl_Obj into a copy of another that is a subcommand
 *	of an ensemble.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Memory is allocated, and the namespace that the ensemble is
 *	built on top of gains another reference.
 *
 *----------------------------------------------------------------------
 */

static void
DupEnsembleCmdRep(objPtr, copyPtr)
    Tcl_Obj *objPtr, *copyPtr;
{
    EnsembleCmdRep *ensembleCmd = (EnsembleCmdRep *)
	    objPtr->internalRep.otherValuePtr;
    EnsembleCmdRep *ensembleCopy = (EnsembleCmdRep *)
	    ckalloc(sizeof(EnsembleCmdRep));
    int length = strlen(ensembleCmd->fullSubcmdName);

    copyPtr->typePtr = &tclEnsembleCmdType;
    copyPtr->internalRep.otherValuePtr = (VOID *) ensembleCopy;
    ensembleCopy->nsPtr = ensembleCmd->nsPtr;
    ensembleCopy->epoch = ensembleCmd->epoch;
    ensembleCopy->nsPtr->refCount++;
    ensembleCopy->realPrefixObj = ensembleCmd->realPrefixObj;
    Tcl_IncrRefCount(ensembleCopy->realPrefixObj);
    ensembleCopy->fullSubcmdName = ckalloc((unsigned) length+1);
    memcpy(ensembleCopy->fullSubcmdName, ensembleCmd->fullSubcmdName,
	    (unsigned) length+1);
}

/*
 *----------------------------------------------------------------------
 *
 * StringOfEnsembleCmdRep --
 *
 *	Creates a string representation of a Tcl_Obj that holds a
 *	subcommand of an ensemble.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	The object gains a string (UTF-8) representation.
 *
 *----------------------------------------------------------------------
 */

static void
StringOfEnsembleCmdRep(objPtr)
    Tcl_Obj *objPtr;
{
    EnsembleCmdRep *ensembleCmd = (EnsembleCmdRep *)
	    objPtr->internalRep.otherValuePtr;
    int length = strlen(ensembleCmd->fullSubcmdName);

    objPtr->length = length;
    objPtr->bytes = ckalloc((unsigned) length+1);
    memcpy(objPtr->bytes, ensembleCmd->fullSubcmdName, (unsigned) length+1);
}
