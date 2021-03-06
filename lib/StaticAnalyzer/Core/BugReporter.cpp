// BugReporter.cpp - Generate PathDiagnostics for Bugs ------------*- C++ -*--//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//  This file defines BugReporter, a utility class for generating
//  PathDiagnostics.
//
//===----------------------------------------------------------------------===//

#include "clang/StaticAnalyzer/Core/BugReporter/BugReporter.h"
#include "clang/StaticAnalyzer/Core/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ExprEngine.h"
#include "clang/AST/ASTContext.h"
#include "clang/Analysis/CFG.h"
#include "clang/AST/DeclObjC.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ParentMap.h"
#include "clang/AST/StmtObjC.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Analysis/ProgramPoint.h"
#include "clang/StaticAnalyzer/Core/BugReporter/PathDiagnostic.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/OwningPtr.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include <queue>

using namespace clang;
using namespace ento;

BugReporterVisitor::~BugReporterVisitor() {}

void BugReporterContext::anchor() {}

//===----------------------------------------------------------------------===//
// Helper routines for walking the ExplodedGraph and fetching statements.
//===----------------------------------------------------------------------===//

static inline const Stmt *GetStmt(const ProgramPoint &P) {
  if (const StmtPoint* SP = dyn_cast<StmtPoint>(&P))
    return SP->getStmt();
  else if (const BlockEdge *BE = dyn_cast<BlockEdge>(&P))
    return BE->getSrc()->getTerminator();
  else if (const CallEnter *CE = dyn_cast<CallEnter>(&P))
    return CE->getCallExpr();
  else if (const CallExitEnd *CEE = dyn_cast<CallExitEnd>(&P))
    return CEE->getCalleeContext()->getCallSite();

  return 0;
}

static inline const ExplodedNode*
GetPredecessorNode(const ExplodedNode *N) {
  return N->pred_empty() ? NULL : *(N->pred_begin());
}

static inline const ExplodedNode*
GetSuccessorNode(const ExplodedNode *N) {
  return N->succ_empty() ? NULL : *(N->succ_begin());
}

static const Stmt *GetPreviousStmt(const ExplodedNode *N) {
  for (N = GetPredecessorNode(N); N; N = GetPredecessorNode(N))
    if (const Stmt *S = GetStmt(N->getLocation()))
      return S;

  return 0;
}

static const Stmt *GetNextStmt(const ExplodedNode *N) {
  for (N = GetSuccessorNode(N); N; N = GetSuccessorNode(N))
    if (const Stmt *S = GetStmt(N->getLocation())) {
      // Check if the statement is '?' or '&&'/'||'.  These are "merges",
      // not actual statement points.
      switch (S->getStmtClass()) {
        case Stmt::ChooseExprClass:
        case Stmt::BinaryConditionalOperatorClass: continue;
        case Stmt::ConditionalOperatorClass: continue;
        case Stmt::BinaryOperatorClass: {
          BinaryOperatorKind Op = cast<BinaryOperator>(S)->getOpcode();
          if (Op == BO_LAnd || Op == BO_LOr)
            continue;
          break;
        }
        default:
          break;
      }
      return S;
    }

  return 0;
}

static inline const Stmt*
GetCurrentOrPreviousStmt(const ExplodedNode *N) {
  if (const Stmt *S = GetStmt(N->getLocation()))
    return S;

  return GetPreviousStmt(N);
}

static inline const Stmt*
GetCurrentOrNextStmt(const ExplodedNode *N) {
  if (const Stmt *S = GetStmt(N->getLocation()))
    return S;

  return GetNextStmt(N);
}

//===----------------------------------------------------------------------===//
// Diagnostic cleanup.
//===----------------------------------------------------------------------===//

/// Recursively scan through a path and prune out calls and macros pieces
/// that aren't needed.  Return true if afterwards the path contains
/// "interesting stuff" which means it should be pruned from the parent path.
bool BugReporter::RemoveUneededCalls(PathPieces &pieces, BugReport *R) {
  bool containsSomethingInteresting = false;
  const unsigned N = pieces.size();
  
  for (unsigned i = 0 ; i < N ; ++i) {
    // Remove the front piece from the path.  If it is still something we
    // want to keep once we are done, we will push it back on the end.
    IntrusiveRefCntPtr<PathDiagnosticPiece> piece(pieces.front());
    pieces.pop_front();
    
    switch (piece->getKind()) {
      case PathDiagnosticPiece::Call: {
        PathDiagnosticCallPiece *call = cast<PathDiagnosticCallPiece>(piece);
        // Check if the location context is interesting.
        assert(LocationContextMap.count(call));
        if (R->isInteresting(LocationContextMap[call])) {
          containsSomethingInteresting = true;
          break;
        }
        // Recursively clean out the subclass.  Keep this call around if
        // it contains any informative diagnostics.
        if (!RemoveUneededCalls(call->path, R))
          continue;
        containsSomethingInteresting = true;
        break;
      }
      case PathDiagnosticPiece::Macro: {
        PathDiagnosticMacroPiece *macro = cast<PathDiagnosticMacroPiece>(piece);
        if (!RemoveUneededCalls(macro->subPieces, R))
          continue;
        containsSomethingInteresting = true;
        break;
      }
      case PathDiagnosticPiece::Event: {
        PathDiagnosticEventPiece *event = cast<PathDiagnosticEventPiece>(piece);
        // We never throw away an event, but we do throw it away wholesale
        // as part of a path if we throw the entire path away.
        containsSomethingInteresting |= !event->isPrunable();
        break;
      }
      case PathDiagnosticPiece::ControlFlow:
        break;
    }
    
    pieces.push_back(piece);
  }
  
  return containsSomethingInteresting;
}

//===----------------------------------------------------------------------===//
// PathDiagnosticBuilder and its associated routines and helper objects.
//===----------------------------------------------------------------------===//

typedef llvm::DenseMap<const ExplodedNode*,
const ExplodedNode*> NodeBackMap;

namespace {
class NodeMapClosure : public BugReport::NodeResolver {
  NodeBackMap& M;
public:
  NodeMapClosure(NodeBackMap *m) : M(*m) {}
  ~NodeMapClosure() {}

  const ExplodedNode *getOriginalNode(const ExplodedNode *N) {
    NodeBackMap::iterator I = M.find(N);
    return I == M.end() ? 0 : I->second;
  }
};

class PathDiagnosticBuilder : public BugReporterContext {
  BugReport *R;
  PathDiagnosticConsumer *PDC;
  OwningPtr<ParentMap> PM;
  NodeMapClosure NMC;
public:
  const LocationContext *LC;
  
  PathDiagnosticBuilder(GRBugReporter &br,
                        BugReport *r, NodeBackMap *Backmap,
                        PathDiagnosticConsumer *pdc)
    : BugReporterContext(br),
      R(r), PDC(pdc), NMC(Backmap), LC(r->getErrorNode()->getLocationContext())
  {}

  PathDiagnosticLocation ExecutionContinues(const ExplodedNode *N);

  PathDiagnosticLocation ExecutionContinues(llvm::raw_string_ostream &os,
                                            const ExplodedNode *N);

  BugReport *getBugReport() { return R; }

  Decl const &getCodeDecl() { return R->getErrorNode()->getCodeDecl(); }
  
  ParentMap& getParentMap() { return LC->getParentMap(); }

  const Stmt *getParent(const Stmt *S) {
    return getParentMap().getParent(S);
  }

  virtual NodeMapClosure& getNodeResolver() { return NMC; }

  PathDiagnosticLocation getEnclosingStmtLocation(const Stmt *S);

  PathDiagnosticConsumer::PathGenerationScheme getGenerationScheme() const {
    return PDC ? PDC->getGenerationScheme() : PathDiagnosticConsumer::Extensive;
  }

  bool supportsLogicalOpControlFlow() const {
    return PDC ? PDC->supportsLogicalOpControlFlow() : true;
  }
};
} // end anonymous namespace

PathDiagnosticLocation
PathDiagnosticBuilder::ExecutionContinues(const ExplodedNode *N) {
  if (const Stmt *S = GetNextStmt(N))
    return PathDiagnosticLocation(S, getSourceManager(), LC);

  return PathDiagnosticLocation::createDeclEnd(N->getLocationContext(),
                                               getSourceManager());
}

PathDiagnosticLocation
PathDiagnosticBuilder::ExecutionContinues(llvm::raw_string_ostream &os,
                                          const ExplodedNode *N) {

  // Slow, but probably doesn't matter.
  if (os.str().empty())
    os << ' ';

  const PathDiagnosticLocation &Loc = ExecutionContinues(N);

  if (Loc.asStmt())
    os << "Execution continues on line "
       << getSourceManager().getExpansionLineNumber(Loc.asLocation())
       << '.';
  else {
    os << "Execution jumps to the end of the ";
    const Decl *D = N->getLocationContext()->getDecl();
    if (isa<ObjCMethodDecl>(D))
      os << "method";
    else if (isa<FunctionDecl>(D))
      os << "function";
    else {
      assert(isa<BlockDecl>(D));
      os << "anonymous block";
    }
    os << '.';
  }

  return Loc;
}

static bool IsNested(const Stmt *S, ParentMap &PM) {
  if (isa<Expr>(S) && PM.isConsumedExpr(cast<Expr>(S)))
    return true;

  const Stmt *Parent = PM.getParentIgnoreParens(S);

  if (Parent)
    switch (Parent->getStmtClass()) {
      case Stmt::ForStmtClass:
      case Stmt::DoStmtClass:
      case Stmt::WhileStmtClass:
        return true;
      default:
        break;
    }

  return false;
}

PathDiagnosticLocation
PathDiagnosticBuilder::getEnclosingStmtLocation(const Stmt *S) {
  assert(S && "Null Stmt *passed to getEnclosingStmtLocation");
  ParentMap &P = getParentMap();
  SourceManager &SMgr = getSourceManager();

  while (IsNested(S, P)) {
    const Stmt *Parent = P.getParentIgnoreParens(S);

    if (!Parent)
      break;

    switch (Parent->getStmtClass()) {
      case Stmt::BinaryOperatorClass: {
        const BinaryOperator *B = cast<BinaryOperator>(Parent);
        if (B->isLogicalOp())
          return PathDiagnosticLocation(S, SMgr, LC);
        break;
      }
      case Stmt::CompoundStmtClass:
      case Stmt::StmtExprClass:
        return PathDiagnosticLocation(S, SMgr, LC);
      case Stmt::ChooseExprClass:
        // Similar to '?' if we are referring to condition, just have the edge
        // point to the entire choose expression.
        if (cast<ChooseExpr>(Parent)->getCond() == S)
          return PathDiagnosticLocation(Parent, SMgr, LC);
        else
          return PathDiagnosticLocation(S, SMgr, LC);
      case Stmt::BinaryConditionalOperatorClass:
      case Stmt::ConditionalOperatorClass:
        // For '?', if we are referring to condition, just have the edge point
        // to the entire '?' expression.
        if (cast<AbstractConditionalOperator>(Parent)->getCond() == S)
          return PathDiagnosticLocation(Parent, SMgr, LC);
        else
          return PathDiagnosticLocation(S, SMgr, LC);
      case Stmt::DoStmtClass:
          return PathDiagnosticLocation(S, SMgr, LC);
      case Stmt::ForStmtClass:
        if (cast<ForStmt>(Parent)->getBody() == S)
          return PathDiagnosticLocation(S, SMgr, LC);
        break;
      case Stmt::IfStmtClass:
        if (cast<IfStmt>(Parent)->getCond() != S)
          return PathDiagnosticLocation(S, SMgr, LC);
        break;
      case Stmt::ObjCForCollectionStmtClass:
        if (cast<ObjCForCollectionStmt>(Parent)->getBody() == S)
          return PathDiagnosticLocation(S, SMgr, LC);
        break;
      case Stmt::WhileStmtClass:
        if (cast<WhileStmt>(Parent)->getCond() != S)
          return PathDiagnosticLocation(S, SMgr, LC);
        break;
      default:
        break;
    }

    S = Parent;
  }

  assert(S && "Cannot have null Stmt for PathDiagnosticLocation");

  // Special case: DeclStmts can appear in for statement declarations, in which
  //  case the ForStmt is the context.
  if (isa<DeclStmt>(S)) {
    if (const Stmt *Parent = P.getParent(S)) {
      switch (Parent->getStmtClass()) {
        case Stmt::ForStmtClass:
        case Stmt::ObjCForCollectionStmtClass:
          return PathDiagnosticLocation(Parent, SMgr, LC);
        default:
          break;
      }
    }
  }
  else if (isa<BinaryOperator>(S)) {
    // Special case: the binary operator represents the initialization
    // code in a for statement (this can happen when the variable being
    // initialized is an old variable.
    if (const ForStmt *FS =
          dyn_cast_or_null<ForStmt>(P.getParentIgnoreParens(S))) {
      if (FS->getInit() == S)
        return PathDiagnosticLocation(FS, SMgr, LC);
    }
  }

  return PathDiagnosticLocation(S, SMgr, LC);
}

//===----------------------------------------------------------------------===//
// "Minimal" path diagnostic generation algorithm.
//===----------------------------------------------------------------------===//
typedef std::pair<PathDiagnosticCallPiece*, const ExplodedNode*> StackDiagPair;
typedef SmallVector<StackDiagPair, 6> StackDiagVector;

static void updateStackPiecesWithMessage(PathDiagnosticPiece *P,
                                         StackDiagVector &CallStack) {
  // If the piece contains a special message, add it to all the call
  // pieces on the active stack.
  if (PathDiagnosticEventPiece *ep =
        dyn_cast<PathDiagnosticEventPiece>(P)) {

    if (ep->hasCallStackHint())
      for (StackDiagVector::iterator I = CallStack.begin(),
                                     E = CallStack.end(); I != E; ++I) {
        PathDiagnosticCallPiece *CP = I->first;
        const ExplodedNode *N = I->second;
        std::string stackMsg = ep->getCallStackMessage(N);

        // The last message on the path to final bug is the most important
        // one. Since we traverse the path backwards, do not add the message
        // if one has been previously added.
        if  (!CP->hasCallStackMessage())
          CP->setCallStackMessage(stackMsg);
      }
  }
}

static void CompactPathDiagnostic(PathPieces &path, const SourceManager& SM);

static void GenerateMinimalPathDiagnostic(PathDiagnostic& PD,
                                          PathDiagnosticBuilder &PDB,
                                          const ExplodedNode *N,
                                      ArrayRef<BugReporterVisitor *> visitors) {

  SourceManager& SMgr = PDB.getSourceManager();
  const LocationContext *LC = PDB.LC;
  const ExplodedNode *NextNode = N->pred_empty()
                                        ? NULL : *(N->pred_begin());

  StackDiagVector CallStack;

  while (NextNode) {
    N = NextNode;
    PDB.LC = N->getLocationContext();
    NextNode = GetPredecessorNode(N);

    ProgramPoint P = N->getLocation();

    do {
      if (const CallExitEnd *CE = dyn_cast<CallExitEnd>(&P)) {
        PathDiagnosticCallPiece *C =
            PathDiagnosticCallPiece::construct(N, *CE, SMgr);
        GRBugReporter& BR = PDB.getBugReporter();
        BR.addCallPieceLocationContextPair(C, CE->getCalleeContext());
        PD.getActivePath().push_front(C);
        PD.pushActivePath(&C->path);
        CallStack.push_back(StackDiagPair(C, N));
        break;
      }

      if (const CallEnter *CE = dyn_cast<CallEnter>(&P)) {
        // Flush all locations, and pop the active path.
        bool VisitedEntireCall = PD.isWithinCall();
        PD.popActivePath();

        // Either we just added a bunch of stuff to the top-level path, or
        // we have a previous CallExitEnd.  If the former, it means that the
        // path terminated within a function call.  We must then take the
        // current contents of the active path and place it within
        // a new PathDiagnosticCallPiece.
        PathDiagnosticCallPiece *C;
        if (VisitedEntireCall) {
          C = cast<PathDiagnosticCallPiece>(PD.getActivePath().front());
        } else {
          const Decl *Caller = CE->getLocationContext()->getDecl();
          C = PathDiagnosticCallPiece::construct(PD.getActivePath(), Caller);
          GRBugReporter& BR = PDB.getBugReporter();
          BR.addCallPieceLocationContextPair(C, CE->getCalleeContext());
        }

        C->setCallee(*CE, SMgr);
        if (!CallStack.empty()) {
          assert(CallStack.back().first == C);
          CallStack.pop_back();
        }
        break;
      }

      if (const BlockEdge *BE = dyn_cast<BlockEdge>(&P)) {
        const CFGBlock *Src = BE->getSrc();
        const CFGBlock *Dst = BE->getDst();
        const Stmt *T = Src->getTerminator();

        if (!T)
          break;

        PathDiagnosticLocation Start =
            PathDiagnosticLocation::createBegin(T, SMgr,
                N->getLocationContext());

        switch (T->getStmtClass()) {
        default:
          break;

        case Stmt::GotoStmtClass:
        case Stmt::IndirectGotoStmtClass: {
          const Stmt *S = GetNextStmt(N);

          if (!S)
            break;

          std::string sbuf;
          llvm::raw_string_ostream os(sbuf);
          const PathDiagnosticLocation &End = PDB.getEnclosingStmtLocation(S);

          os << "Control jumps to line "
              << End.asLocation().getExpansionLineNumber();
          PD.getActivePath().push_front(new PathDiagnosticControlFlowPiece(
              Start, End, os.str()));
          break;
        }

        case Stmt::SwitchStmtClass: {
          // Figure out what case arm we took.
          std::string sbuf;
          llvm::raw_string_ostream os(sbuf);

          if (const Stmt *S = Dst->getLabel()) {
            PathDiagnosticLocation End(S, SMgr, LC);

            switch (S->getStmtClass()) {
            default:
              os << "No cases match in the switch statement. "
              "Control jumps to line "
              << End.asLocation().getExpansionLineNumber();
              break;
            case Stmt::DefaultStmtClass:
              os << "Control jumps to the 'default' case at line "
              << End.asLocation().getExpansionLineNumber();
              break;

            case Stmt::CaseStmtClass: {
              os << "Control jumps to 'case ";
              const CaseStmt *Case = cast<CaseStmt>(S);
              const Expr *LHS = Case->getLHS()->IgnoreParenCasts();

              // Determine if it is an enum.
              bool GetRawInt = true;

              if (const DeclRefExpr *DR = dyn_cast<DeclRefExpr>(LHS)) {
                // FIXME: Maybe this should be an assertion.  Are there cases
                // were it is not an EnumConstantDecl?
                const EnumConstantDecl *D =
                    dyn_cast<EnumConstantDecl>(DR->getDecl());

                if (D) {
                  GetRawInt = false;
                  os << *D;
                }
              }

              if (GetRawInt)
                os << LHS->EvaluateKnownConstInt(PDB.getASTContext());

              os << ":'  at line "
                  << End.asLocation().getExpansionLineNumber();
              break;
            }
            }
            PD.getActivePath().push_front(new PathDiagnosticControlFlowPiece(
                Start, End, os.str()));
          }
          else {
            os << "'Default' branch taken. ";
            const PathDiagnosticLocation &End = PDB.ExecutionContinues(os, N);
            PD.getActivePath().push_front(new PathDiagnosticControlFlowPiece(
                Start, End, os.str()));
          }

          break;
        }

        case Stmt::BreakStmtClass:
        case Stmt::ContinueStmtClass: {
          std::string sbuf;
          llvm::raw_string_ostream os(sbuf);
          PathDiagnosticLocation End = PDB.ExecutionContinues(os, N);
          PD.getActivePath().push_front(new PathDiagnosticControlFlowPiece(
              Start, End, os.str()));
          break;
        }

        // Determine control-flow for ternary '?'.
        case Stmt::BinaryConditionalOperatorClass:
        case Stmt::ConditionalOperatorClass: {
          std::string sbuf;
          llvm::raw_string_ostream os(sbuf);
          os << "'?' condition is ";

          if (*(Src->succ_begin()+1) == Dst)
            os << "false";
          else
            os << "true";

          PathDiagnosticLocation End = PDB.ExecutionContinues(N);

          if (const Stmt *S = End.asStmt())
            End = PDB.getEnclosingStmtLocation(S);

          PD.getActivePath().push_front(new PathDiagnosticControlFlowPiece(
              Start, End, os.str()));
          break;
        }

        // Determine control-flow for short-circuited '&&' and '||'.
        case Stmt::BinaryOperatorClass: {
          if (!PDB.supportsLogicalOpControlFlow())
            break;

          const BinaryOperator *B = cast<BinaryOperator>(T);
          std::string sbuf;
          llvm::raw_string_ostream os(sbuf);
          os << "Left side of '";

          if (B->getOpcode() == BO_LAnd) {
            os << "&&" << "' is ";

            if (*(Src->succ_begin()+1) == Dst) {
              os << "false";
              PathDiagnosticLocation End(B->getLHS(), SMgr, LC);
              PathDiagnosticLocation Start =
                  PathDiagnosticLocation::createOperatorLoc(B, SMgr);
              PD.getActivePath().push_front(new PathDiagnosticControlFlowPiece(
                  Start, End, os.str()));
            }
            else {
              os << "true";
              PathDiagnosticLocation Start(B->getLHS(), SMgr, LC);
              PathDiagnosticLocation End = PDB.ExecutionContinues(N);
              PD.getActivePath().push_front(new PathDiagnosticControlFlowPiece(
                  Start, End, os.str()));
            }
          }
          else {
            assert(B->getOpcode() == BO_LOr);
            os << "||" << "' is ";

            if (*(Src->succ_begin()+1) == Dst) {
              os << "false";
              PathDiagnosticLocation Start(B->getLHS(), SMgr, LC);
              PathDiagnosticLocation End = PDB.ExecutionContinues(N);
              PD.getActivePath().push_front(new PathDiagnosticControlFlowPiece(
                  Start, End, os.str()));
            }
            else {
              os << "true";
              PathDiagnosticLocation End(B->getLHS(), SMgr, LC);
              PathDiagnosticLocation Start =
                  PathDiagnosticLocation::createOperatorLoc(B, SMgr);
              PD.getActivePath().push_front(new PathDiagnosticControlFlowPiece(
                  Start, End, os.str()));
            }
          }

          break;
        }

        case Stmt::DoStmtClass:  {
          if (*(Src->succ_begin()) == Dst) {
            std::string sbuf;
            llvm::raw_string_ostream os(sbuf);

            os << "Loop condition is true. ";
            PathDiagnosticLocation End = PDB.ExecutionContinues(os, N);

            if (const Stmt *S = End.asStmt())
              End = PDB.getEnclosingStmtLocation(S);

            PD.getActivePath().push_front(new PathDiagnosticControlFlowPiece(
                Start, End, os.str()));
          }
          else {
            PathDiagnosticLocation End = PDB.ExecutionContinues(N);

            if (const Stmt *S = End.asStmt())
              End = PDB.getEnclosingStmtLocation(S);

            PD.getActivePath().push_front(new PathDiagnosticControlFlowPiece(
                Start, End, "Loop condition is false.  Exiting loop"));
          }

          break;
        }

        case Stmt::WhileStmtClass:
        case Stmt::ForStmtClass: {
          if (*(Src->succ_begin()+1) == Dst) {
            std::string sbuf;
            llvm::raw_string_ostream os(sbuf);

            os << "Loop condition is false. ";
            PathDiagnosticLocation End = PDB.ExecutionContinues(os, N);
            if (const Stmt *S = End.asStmt())
              End = PDB.getEnclosingStmtLocation(S);

            PD.getActivePath().push_front(new PathDiagnosticControlFlowPiece(
                Start, End, os.str()));
          }
          else {
            PathDiagnosticLocation End = PDB.ExecutionContinues(N);
            if (const Stmt *S = End.asStmt())
              End = PDB.getEnclosingStmtLocation(S);

            PD.getActivePath().push_front(new PathDiagnosticControlFlowPiece(
                Start, End, "Loop condition is true.  Entering loop body"));
          }

          break;
        }

        case Stmt::IfStmtClass: {
          PathDiagnosticLocation End = PDB.ExecutionContinues(N);

          if (const Stmt *S = End.asStmt())
            End = PDB.getEnclosingStmtLocation(S);

          if (*(Src->succ_begin()+1) == Dst)
            PD.getActivePath().push_front(new PathDiagnosticControlFlowPiece(
                Start, End, "Taking false branch"));
          else
            PD.getActivePath().push_front(new PathDiagnosticControlFlowPiece(
                Start, End, "Taking true branch"));

          break;
        }
        }
      }
    } while(0);

    if (NextNode) {
      // Add diagnostic pieces from custom visitors.
      BugReport *R = PDB.getBugReport();
      for (ArrayRef<BugReporterVisitor *>::iterator I = visitors.begin(),
                                                    E = visitors.end();
           I != E; ++I) {
        if (PathDiagnosticPiece *p = (*I)->VisitNode(N, NextNode, PDB, *R)) {
          PD.getActivePath().push_front(p);
          updateStackPiecesWithMessage(p, CallStack);
        }
      }
    }
  }

  // After constructing the full PathDiagnostic, do a pass over it to compact
  // PathDiagnosticPieces that occur within a macro.
  CompactPathDiagnostic(PD.getMutablePieces(), PDB.getSourceManager());
}

//===----------------------------------------------------------------------===//
// "Extensive" PathDiagnostic generation.
//===----------------------------------------------------------------------===//

static bool IsControlFlowExpr(const Stmt *S) {
  const Expr *E = dyn_cast<Expr>(S);

  if (!E)
    return false;

  E = E->IgnoreParenCasts();

  if (isa<AbstractConditionalOperator>(E))
    return true;

  if (const BinaryOperator *B = dyn_cast<BinaryOperator>(E))
    if (B->isLogicalOp())
      return true;

  return false;
}

namespace {
class ContextLocation : public PathDiagnosticLocation {
  bool IsDead;
public:
  ContextLocation(const PathDiagnosticLocation &L, bool isdead = false)
    : PathDiagnosticLocation(L), IsDead(isdead) {}

  void markDead() { IsDead = true; }
  bool isDead() const { return IsDead; }
};

class EdgeBuilder {
  std::vector<ContextLocation> CLocs;
  typedef std::vector<ContextLocation>::iterator iterator;
  PathDiagnostic &PD;
  PathDiagnosticBuilder &PDB;
  PathDiagnosticLocation PrevLoc;

  bool IsConsumedExpr(const PathDiagnosticLocation &L);

  bool containsLocation(const PathDiagnosticLocation &Container,
                        const PathDiagnosticLocation &Containee);

  PathDiagnosticLocation getContextLocation(const PathDiagnosticLocation &L);

  PathDiagnosticLocation cleanUpLocation(PathDiagnosticLocation L,
                                         bool firstCharOnly = false) {
    if (const Stmt *S = L.asStmt()) {
      const Stmt *Original = S;
      while (1) {
        // Adjust the location for some expressions that are best referenced
        // by one of their subexpressions.
        switch (S->getStmtClass()) {
          default:
            break;
          case Stmt::ParenExprClass:
          case Stmt::GenericSelectionExprClass:
            S = cast<Expr>(S)->IgnoreParens();
            firstCharOnly = true;
            continue;
          case Stmt::BinaryConditionalOperatorClass:
          case Stmt::ConditionalOperatorClass:
            S = cast<AbstractConditionalOperator>(S)->getCond();
            firstCharOnly = true;
            continue;
          case Stmt::ChooseExprClass:
            S = cast<ChooseExpr>(S)->getCond();
            firstCharOnly = true;
            continue;
          case Stmt::BinaryOperatorClass:
            S = cast<BinaryOperator>(S)->getLHS();
            firstCharOnly = true;
            continue;
        }

        break;
      }

      if (S != Original)
        L = PathDiagnosticLocation(S, L.getManager(), PDB.LC);
    }

    if (firstCharOnly)
      L  = PathDiagnosticLocation::createSingleLocation(L);

    return L;
  }

  void popLocation() {
    if (!CLocs.back().isDead() && CLocs.back().asLocation().isFileID()) {
      // For contexts, we only one the first character as the range.
      rawAddEdge(cleanUpLocation(CLocs.back(), true));
    }
    CLocs.pop_back();
  }

public:
  EdgeBuilder(PathDiagnostic &pd, PathDiagnosticBuilder &pdb)
    : PD(pd), PDB(pdb) {

      // If the PathDiagnostic already has pieces, add the enclosing statement
      // of the first piece as a context as well.
      if (!PD.path.empty()) {
        PrevLoc = (*PD.path.begin())->getLocation();

        if (const Stmt *S = PrevLoc.asStmt())
          addExtendedContext(PDB.getEnclosingStmtLocation(S).asStmt());
      }
  }

  ~EdgeBuilder() {
    while (!CLocs.empty()) popLocation();
    
    // Finally, add an initial edge from the start location of the first
    // statement (if it doesn't already exist).
    PathDiagnosticLocation L = PathDiagnosticLocation::createDeclBegin(
                                                       PDB.LC,
                                                       PDB.getSourceManager());
    if (L.isValid())
      rawAddEdge(L);
  }

  void flushLocations() {
    while (!CLocs.empty())
      popLocation();
    PrevLoc = PathDiagnosticLocation();
  }
  
  void addEdge(PathDiagnosticLocation NewLoc, bool alwaysAdd = false);

  void rawAddEdge(PathDiagnosticLocation NewLoc);

  void addContext(const Stmt *S);
  void addContext(const PathDiagnosticLocation &L);
  void addExtendedContext(const Stmt *S);
};
} // end anonymous namespace


PathDiagnosticLocation
EdgeBuilder::getContextLocation(const PathDiagnosticLocation &L) {
  if (const Stmt *S = L.asStmt()) {
    if (IsControlFlowExpr(S))
      return L;

    return PDB.getEnclosingStmtLocation(S);
  }

  return L;
}

bool EdgeBuilder::containsLocation(const PathDiagnosticLocation &Container,
                                   const PathDiagnosticLocation &Containee) {

  if (Container == Containee)
    return true;

  if (Container.asDecl())
    return true;

  if (const Stmt *S = Containee.asStmt())
    if (const Stmt *ContainerS = Container.asStmt()) {
      while (S) {
        if (S == ContainerS)
          return true;
        S = PDB.getParent(S);
      }
      return false;
    }

  // Less accurate: compare using source ranges.
  SourceRange ContainerR = Container.asRange();
  SourceRange ContaineeR = Containee.asRange();

  SourceManager &SM = PDB.getSourceManager();
  SourceLocation ContainerRBeg = SM.getExpansionLoc(ContainerR.getBegin());
  SourceLocation ContainerREnd = SM.getExpansionLoc(ContainerR.getEnd());
  SourceLocation ContaineeRBeg = SM.getExpansionLoc(ContaineeR.getBegin());
  SourceLocation ContaineeREnd = SM.getExpansionLoc(ContaineeR.getEnd());

  unsigned ContainerBegLine = SM.getExpansionLineNumber(ContainerRBeg);
  unsigned ContainerEndLine = SM.getExpansionLineNumber(ContainerREnd);
  unsigned ContaineeBegLine = SM.getExpansionLineNumber(ContaineeRBeg);
  unsigned ContaineeEndLine = SM.getExpansionLineNumber(ContaineeREnd);

  assert(ContainerBegLine <= ContainerEndLine);
  assert(ContaineeBegLine <= ContaineeEndLine);

  return (ContainerBegLine <= ContaineeBegLine &&
          ContainerEndLine >= ContaineeEndLine &&
          (ContainerBegLine != ContaineeBegLine ||
           SM.getExpansionColumnNumber(ContainerRBeg) <=
           SM.getExpansionColumnNumber(ContaineeRBeg)) &&
          (ContainerEndLine != ContaineeEndLine ||
           SM.getExpansionColumnNumber(ContainerREnd) >=
           SM.getExpansionColumnNumber(ContaineeREnd)));
}

void EdgeBuilder::rawAddEdge(PathDiagnosticLocation NewLoc) {
  if (!PrevLoc.isValid()) {
    PrevLoc = NewLoc;
    return;
  }

  const PathDiagnosticLocation &NewLocClean = cleanUpLocation(NewLoc);
  const PathDiagnosticLocation &PrevLocClean = cleanUpLocation(PrevLoc);

  if (NewLocClean.asLocation() == PrevLocClean.asLocation())
    return;

  // FIXME: Ignore intra-macro edges for now.
  if (NewLocClean.asLocation().getExpansionLoc() ==
      PrevLocClean.asLocation().getExpansionLoc())
    return;

  PD.getActivePath().push_front(new PathDiagnosticControlFlowPiece(NewLocClean, PrevLocClean));
  PrevLoc = NewLoc;
}

void EdgeBuilder::addEdge(PathDiagnosticLocation NewLoc, bool alwaysAdd) {

  if (!alwaysAdd && NewLoc.asLocation().isMacroID())
    return;

  const PathDiagnosticLocation &CLoc = getContextLocation(NewLoc);

  while (!CLocs.empty()) {
    ContextLocation &TopContextLoc = CLocs.back();

    // Is the top location context the same as the one for the new location?
    if (TopContextLoc == CLoc) {
      if (alwaysAdd) {
        if (IsConsumedExpr(TopContextLoc) &&
            !IsControlFlowExpr(TopContextLoc.asStmt()))
            TopContextLoc.markDead();

        rawAddEdge(NewLoc);
      }

      return;
    }

    if (containsLocation(TopContextLoc, CLoc)) {
      if (alwaysAdd) {
        rawAddEdge(NewLoc);

        if (IsConsumedExpr(CLoc) && !IsControlFlowExpr(CLoc.asStmt())) {
          CLocs.push_back(ContextLocation(CLoc, true));
          return;
        }
      }

      CLocs.push_back(CLoc);
      return;
    }

    // Context does not contain the location.  Flush it.
    popLocation();
  }

  // If we reach here, there is no enclosing context.  Just add the edge.
  rawAddEdge(NewLoc);
}

bool EdgeBuilder::IsConsumedExpr(const PathDiagnosticLocation &L) {
  if (const Expr *X = dyn_cast_or_null<Expr>(L.asStmt()))
    return PDB.getParentMap().isConsumedExpr(X) && !IsControlFlowExpr(X);

  return false;
}

void EdgeBuilder::addExtendedContext(const Stmt *S) {
  if (!S)
    return;

  const Stmt *Parent = PDB.getParent(S);
  while (Parent) {
    if (isa<CompoundStmt>(Parent))
      Parent = PDB.getParent(Parent);
    else
      break;
  }

  if (Parent) {
    switch (Parent->getStmtClass()) {
      case Stmt::DoStmtClass:
      case Stmt::ObjCAtSynchronizedStmtClass:
        addContext(Parent);
      default:
        break;
    }
  }

  addContext(S);
}

void EdgeBuilder::addContext(const Stmt *S) {
  if (!S)
    return;

  PathDiagnosticLocation L(S, PDB.getSourceManager(), PDB.LC);
  addContext(L);
}

void EdgeBuilder::addContext(const PathDiagnosticLocation &L) {
  while (!CLocs.empty()) {
    const PathDiagnosticLocation &TopContextLoc = CLocs.back();

    // Is the top location context the same as the one for the new location?
    if (TopContextLoc == L)
      return;

    if (containsLocation(TopContextLoc, L)) {
      CLocs.push_back(L);
      return;
    }

    // Context does not contain the location.  Flush it.
    popLocation();
  }

  CLocs.push_back(L);
}

// Cone-of-influence: support the reverse propagation of "interesting" symbols
// and values by tracing interesting calculations backwards through evaluated
// expressions along a path.  This is probably overly complicated, but the idea
// is that if an expression computed an "interesting" value, the child
// expressions are are also likely to be "interesting" as well (which then
// propagates to the values they in turn compute).  This reverse propagation
// is needed to track interesting correlations across function call boundaries,
// where formal arguments bind to actual arguments, etc.  This is also needed
// because the constraint solver sometimes simplifies certain symbolic values
// into constants when appropriate, and this complicates reasoning about
// interesting values.
typedef llvm::DenseSet<const Expr *> InterestingExprs;

static void reversePropagateIntererstingSymbols(BugReport &R,
                                                InterestingExprs &IE,
                                                const ProgramState *State,
                                                const Expr *Ex,
                                                const LocationContext *LCtx) {
  SVal V = State->getSVal(Ex, LCtx);
  if (!(R.isInteresting(V) || IE.count(Ex)))
    return;
  
  switch (Ex->getStmtClass()) {
    default:
      if (!isa<CastExpr>(Ex))
        break;
      // Fall through.
    case Stmt::BinaryOperatorClass:
    case Stmt::UnaryOperatorClass: {
      for (Stmt::const_child_iterator CI = Ex->child_begin(),
            CE = Ex->child_end();
            CI != CE; ++CI) {
        if (const Expr *child = dyn_cast_or_null<Expr>(*CI)) {
          IE.insert(child);
          SVal ChildV = State->getSVal(child, LCtx);
          R.markInteresting(ChildV);
        }
        break;
      }
    }
  }
  
  R.markInteresting(V);
}

static void reversePropagateInterestingSymbols(BugReport &R,
                                               InterestingExprs &IE,
                                               const ProgramState *State,
                                               const LocationContext *CalleeCtx,
                                               const LocationContext *CallerCtx)
{
  // FIXME: Handle non-CallExpr-based CallEvents.
  const StackFrameContext *Callee = CalleeCtx->getCurrentStackFrame();
  const Stmt *CallSite = Callee->getCallSite();
  if (const CallExpr *CE = dyn_cast_or_null<CallExpr>(CallSite)) {
    if (const FunctionDecl *FD = dyn_cast<FunctionDecl>(CalleeCtx->getDecl())) {
      FunctionDecl::param_const_iterator PI = FD->param_begin(), 
                                         PE = FD->param_end();
      CallExpr::const_arg_iterator AI = CE->arg_begin(), AE = CE->arg_end();
      for (; AI != AE && PI != PE; ++AI, ++PI) {
        if (const Expr *ArgE = *AI) {
          if (const ParmVarDecl *PD = *PI) {
            Loc LV = State->getLValue(PD, CalleeCtx);
            if (R.isInteresting(LV) || R.isInteresting(State->getRawSVal(LV)))
              IE.insert(ArgE);
          }
        }
      }
    }
  }
}
                                               
static void GenerateExtensivePathDiagnostic(PathDiagnostic& PD,
                                            PathDiagnosticBuilder &PDB,
                                            const ExplodedNode *N,
                                      ArrayRef<BugReporterVisitor *> visitors) {
  EdgeBuilder EB(PD, PDB);
  const SourceManager& SM = PDB.getSourceManager();
  StackDiagVector CallStack;
  InterestingExprs IE;

  const ExplodedNode *NextNode = N->pred_empty() ? NULL : *(N->pred_begin());
  while (NextNode) {
    N = NextNode;
    NextNode = GetPredecessorNode(N);
    ProgramPoint P = N->getLocation();

    do {
      if (const PostStmt *PS = dyn_cast<PostStmt>(&P)) {
        if (const Expr *Ex = PS->getStmtAs<Expr>())
          reversePropagateIntererstingSymbols(*PDB.getBugReport(), IE,
                                              N->getState().getPtr(), Ex,
                                              N->getLocationContext());
      }
      
      if (const CallExitEnd *CE = dyn_cast<CallExitEnd>(&P)) {
        const Stmt *S = CE->getCalleeContext()->getCallSite();
        if (const Expr *Ex = dyn_cast_or_null<Expr>(S)) {
            reversePropagateIntererstingSymbols(*PDB.getBugReport(), IE,
                                                N->getState().getPtr(), Ex,
                                                N->getLocationContext());
        }
        
        PathDiagnosticCallPiece *C =
          PathDiagnosticCallPiece::construct(N, *CE, SM);
        GRBugReporter& BR = PDB.getBugReporter();
        BR.addCallPieceLocationContextPair(C, CE->getCalleeContext());

        EB.addEdge(C->callReturn, true);
        EB.flushLocations();

        PD.getActivePath().push_front(C);
        PD.pushActivePath(&C->path);
        CallStack.push_back(StackDiagPair(C, N));
        break;
      }
      
      // Pop the call hierarchy if we are done walking the contents
      // of a function call.
      if (const CallEnter *CE = dyn_cast<CallEnter>(&P)) {
        // Add an edge to the start of the function.
        const Decl *D = CE->getCalleeContext()->getDecl();
        PathDiagnosticLocation pos =
          PathDiagnosticLocation::createBegin(D, SM);
        EB.addEdge(pos);
        
        // Flush all locations, and pop the active path.
        bool VisitedEntireCall = PD.isWithinCall();
        EB.flushLocations();
        PD.popActivePath();
        PDB.LC = N->getLocationContext();

        // Either we just added a bunch of stuff to the top-level path, or
        // we have a previous CallExitEnd.  If the former, it means that the
        // path terminated within a function call.  We must then take the
        // current contents of the active path and place it within
        // a new PathDiagnosticCallPiece.
        PathDiagnosticCallPiece *C;
        if (VisitedEntireCall) {
          C = cast<PathDiagnosticCallPiece>(PD.getActivePath().front());
        } else {
          const Decl *Caller = CE->getLocationContext()->getDecl();
          C = PathDiagnosticCallPiece::construct(PD.getActivePath(), Caller);
          GRBugReporter& BR = PDB.getBugReporter();
          BR.addCallPieceLocationContextPair(C, CE->getCalleeContext());
        }

        C->setCallee(*CE, SM);
        EB.addContext(C->getLocation());

        if (!CallStack.empty()) {
          assert(CallStack.back().first == C);
          CallStack.pop_back();
        }
        break;
      }
      
      // Note that is important that we update the LocationContext
      // after looking at CallExits.  CallExit basically adds an
      // edge in the *caller*, so we don't want to update the LocationContext
      // too soon.
      PDB.LC = N->getLocationContext();

      // Block edges.
      if (const BlockEdge *BE = dyn_cast<BlockEdge>(&P)) {
        // Does this represent entering a call?  If so, look at propagating
        // interesting symbols across call boundaries.
        if (NextNode) {
          const LocationContext *CallerCtx = NextNode->getLocationContext();
          const LocationContext *CalleeCtx = PDB.LC;
          if (CallerCtx != CalleeCtx) {
            reversePropagateInterestingSymbols(*PDB.getBugReport(), IE,
                                               N->getState().getPtr(),
                                               CalleeCtx, CallerCtx);
          }
        }
       
        const CFGBlock &Blk = *BE->getSrc();
        const Stmt *Term = Blk.getTerminator();

        // Are we jumping to the head of a loop?  Add a special diagnostic.
        if (const Stmt *Loop = BE->getDst()->getLoopTarget()) {
          PathDiagnosticLocation L(Loop, SM, PDB.LC);
          const CompoundStmt *CS = NULL;

          if (!Term) {
            if (const ForStmt *FS = dyn_cast<ForStmt>(Loop))
              CS = dyn_cast<CompoundStmt>(FS->getBody());
            else if (const WhileStmt *WS = dyn_cast<WhileStmt>(Loop))
              CS = dyn_cast<CompoundStmt>(WS->getBody());
          }

          PathDiagnosticEventPiece *p =
            new PathDiagnosticEventPiece(L,
                                        "Looping back to the head of the loop");
          p->setPrunable(true);

          EB.addEdge(p->getLocation(), true);
          PD.getActivePath().push_front(p);

          if (CS) {
            PathDiagnosticLocation BL =
              PathDiagnosticLocation::createEndBrace(CS, SM);
            EB.addEdge(BL);
          }
        }

        if (Term)
          EB.addContext(Term);

        break;
      }

      if (const BlockEntrance *BE = dyn_cast<BlockEntrance>(&P)) {
        if (const CFGStmt *S = BE->getFirstElement().getAs<CFGStmt>()) {
          const Stmt *stmt = S->getStmt();
          if (IsControlFlowExpr(stmt)) {
            // Add the proper context for '&&', '||', and '?'.
            EB.addContext(stmt);
          }
          else
            EB.addExtendedContext(PDB.getEnclosingStmtLocation(stmt).asStmt());
        }
        
        break;
      }
      
      
    } while (0);

    if (!NextNode)
      continue;

    // Add pieces from custom visitors.
    BugReport *R = PDB.getBugReport();
    for (ArrayRef<BugReporterVisitor *>::iterator I = visitors.begin(),
                                                  E = visitors.end();
         I != E; ++I) {
      if (PathDiagnosticPiece *p = (*I)->VisitNode(N, NextNode, PDB, *R)) {
        const PathDiagnosticLocation &Loc = p->getLocation();
        EB.addEdge(Loc, true);
        PD.getActivePath().push_front(p);
        updateStackPiecesWithMessage(p, CallStack);

        if (const Stmt *S = Loc.asStmt())
          EB.addExtendedContext(PDB.getEnclosingStmtLocation(S).asStmt());
      }
    }
  }
}

//===----------------------------------------------------------------------===//
// Methods for BugType and subclasses.
//===----------------------------------------------------------------------===//
BugType::~BugType() { }

void BugType::FlushReports(BugReporter &BR) {}

void BuiltinBug::anchor() {}

//===----------------------------------------------------------------------===//
// Methods for BugReport and subclasses.
//===----------------------------------------------------------------------===//

void BugReport::NodeResolver::anchor() {}

void BugReport::addVisitor(BugReporterVisitor* visitor) {
  if (!visitor)
    return;

  llvm::FoldingSetNodeID ID;
  visitor->Profile(ID);
  void *InsertPos;

  if (CallbacksSet.FindNodeOrInsertPos(ID, InsertPos)) {
    delete visitor;
    return;
  }

  CallbacksSet.InsertNode(visitor, InsertPos);
  Callbacks.push_back(visitor);
  ++ConfigurationChangeToken;
}

BugReport::~BugReport() {
  for (visitor_iterator I = visitor_begin(), E = visitor_end(); I != E; ++I) {
    delete *I;
  }
  while (!interestingSymbols.empty()) {
    popInterestingSymbolsAndRegions();
  }
}

const Decl *BugReport::getDeclWithIssue() const {
  if (DeclWithIssue)
    return DeclWithIssue;
  
  const ExplodedNode *N = getErrorNode();
  if (!N)
    return 0;
  
  const LocationContext *LC = N->getLocationContext();
  return LC->getCurrentStackFrame()->getDecl();
}

void BugReport::Profile(llvm::FoldingSetNodeID& hash) const {
  hash.AddPointer(&BT);
  hash.AddString(Description);
  if (UniqueingLocation.isValid()) {
    UniqueingLocation.Profile(hash);
  } else if (Location.isValid()) {
    Location.Profile(hash);
  } else {
    assert(ErrorNode);
    hash.AddPointer(GetCurrentOrPreviousStmt(ErrorNode));
  }

  for (SmallVectorImpl<SourceRange>::const_iterator I =
      Ranges.begin(), E = Ranges.end(); I != E; ++I) {
    const SourceRange range = *I;
    if (!range.isValid())
      continue;
    hash.AddInteger(range.getBegin().getRawEncoding());
    hash.AddInteger(range.getEnd().getRawEncoding());
  }
}

void BugReport::markInteresting(SymbolRef sym) {
  if (!sym)
    return;

  // If the symbol wasn't already in our set, note a configuration change.
  if (getInterestingSymbols().insert(sym).second)
    ++ConfigurationChangeToken;

  if (const SymbolMetadata *meta = dyn_cast<SymbolMetadata>(sym))
    getInterestingRegions().insert(meta->getRegion());
}

void BugReport::markInteresting(const MemRegion *R) {
  if (!R)
    return;

  // If the base region wasn't already in our set, note a configuration change.
  R = R->getBaseRegion();
  if (getInterestingRegions().insert(R).second)
    ++ConfigurationChangeToken;

  if (const SymbolicRegion *SR = dyn_cast<SymbolicRegion>(R))
    getInterestingSymbols().insert(SR->getSymbol());
}

void BugReport::markInteresting(SVal V) {
  markInteresting(V.getAsRegion());
  markInteresting(V.getAsSymbol());
}

void BugReport::markInteresting(const LocationContext *LC) {
  if (!LC)
    return;
  InterestingLocationContexts.insert(LC);
}

bool BugReport::isInteresting(SVal V) {
  return isInteresting(V.getAsRegion()) || isInteresting(V.getAsSymbol());
}

bool BugReport::isInteresting(SymbolRef sym) {
  if (!sym)
    return false;
  // We don't currently consider metadata symbols to be interesting
  // even if we know their region is interesting. Is that correct behavior?
  return getInterestingSymbols().count(sym);
}

bool BugReport::isInteresting(const MemRegion *R) {
  if (!R)
    return false;
  R = R->getBaseRegion();
  bool b = getInterestingRegions().count(R);
  if (b)
    return true;
  if (const SymbolicRegion *SR = dyn_cast<SymbolicRegion>(R))
    return getInterestingSymbols().count(SR->getSymbol());
  return false;
}

bool BugReport::isInteresting(const LocationContext *LC) {
  if (!LC)
    return false;
  return InterestingLocationContexts.count(LC);
}

void BugReport::lazyInitializeInterestingSets() {
  if (interestingSymbols.empty()) {
    interestingSymbols.push_back(new Symbols());
    interestingRegions.push_back(new Regions());
  }
}

BugReport::Symbols &BugReport::getInterestingSymbols() {
  lazyInitializeInterestingSets();
  return *interestingSymbols.back();
}

BugReport::Regions &BugReport::getInterestingRegions() {
  lazyInitializeInterestingSets();
  return *interestingRegions.back();
}

void BugReport::pushInterestingSymbolsAndRegions() {
  interestingSymbols.push_back(new Symbols(getInterestingSymbols()));
  interestingRegions.push_back(new Regions(getInterestingRegions()));
}

void BugReport::popInterestingSymbolsAndRegions() {
  delete interestingSymbols.back();
  interestingSymbols.pop_back();
  delete interestingRegions.back();
  interestingRegions.pop_back();
}

const Stmt *BugReport::getStmt() const {
  if (!ErrorNode)
    return 0;

  ProgramPoint ProgP = ErrorNode->getLocation();
  const Stmt *S = NULL;

  if (BlockEntrance *BE = dyn_cast<BlockEntrance>(&ProgP)) {
    CFGBlock &Exit = ProgP.getLocationContext()->getCFG()->getExit();
    if (BE->getBlock() == &Exit)
      S = GetPreviousStmt(ErrorNode);
  }
  if (!S)
    S = GetStmt(ProgP);

  return S;
}

std::pair<BugReport::ranges_iterator, BugReport::ranges_iterator>
BugReport::getRanges() {
    // If no custom ranges, add the range of the statement corresponding to
    // the error node.
    if (Ranges.empty()) {
      if (const Expr *E = dyn_cast_or_null<Expr>(getStmt()))
        addRange(E->getSourceRange());
      else
        return std::make_pair(ranges_iterator(), ranges_iterator());
    }

    // User-specified absence of range info.
    if (Ranges.size() == 1 && !Ranges.begin()->isValid())
      return std::make_pair(ranges_iterator(), ranges_iterator());

    return std::make_pair(Ranges.begin(), Ranges.end());
}

PathDiagnosticLocation BugReport::getLocation(const SourceManager &SM) const {
  if (ErrorNode) {
    assert(!Location.isValid() &&
     "Either Location or ErrorNode should be specified but not both.");

    if (const Stmt *S = GetCurrentOrPreviousStmt(ErrorNode)) {
      const LocationContext *LC = ErrorNode->getLocationContext();

      // For member expressions, return the location of the '.' or '->'.
      if (const MemberExpr *ME = dyn_cast<MemberExpr>(S))
        return PathDiagnosticLocation::createMemberLoc(ME, SM);
      // For binary operators, return the location of the operator.
      if (const BinaryOperator *B = dyn_cast<BinaryOperator>(S))
        return PathDiagnosticLocation::createOperatorLoc(B, SM);

      return PathDiagnosticLocation::createBegin(S, SM, LC);
    }
  } else {
    assert(Location.isValid());
    return Location;
  }

  return PathDiagnosticLocation();
}

//===----------------------------------------------------------------------===//
// Methods for BugReporter and subclasses.
//===----------------------------------------------------------------------===//

BugReportEquivClass::~BugReportEquivClass() { }
GRBugReporter::~GRBugReporter() { }
BugReporterData::~BugReporterData() {}

ExplodedGraph &GRBugReporter::getGraph() { return Eng.getGraph(); }

ProgramStateManager&
GRBugReporter::getStateManager() { return Eng.getStateManager(); }

BugReporter::~BugReporter() {
  FlushReports();

  // Free the bug reports we are tracking.
  typedef std::vector<BugReportEquivClass *> ContTy;
  for (ContTy::iterator I = EQClassesVector.begin(), E = EQClassesVector.end();
       I != E; ++I) {
    delete *I;
  }
}

void BugReporter::FlushReports() {
  if (BugTypes.isEmpty())
    return;

  // First flush the warnings for each BugType.  This may end up creating new
  // warnings and new BugTypes.
  // FIXME: Only NSErrorChecker needs BugType's FlushReports.
  // Turn NSErrorChecker into a proper checker and remove this.
  SmallVector<const BugType*, 16> bugTypes;
  for (BugTypesTy::iterator I=BugTypes.begin(), E=BugTypes.end(); I!=E; ++I)
    bugTypes.push_back(*I);
  for (SmallVector<const BugType*, 16>::iterator
         I = bugTypes.begin(), E = bugTypes.end(); I != E; ++I)
    const_cast<BugType*>(*I)->FlushReports(*this);

  // We need to flush reports in deterministic order to ensure the order
  // of the reports is consistent between runs.
  typedef std::vector<BugReportEquivClass *> ContVecTy;
  for (ContVecTy::iterator EI=EQClassesVector.begin(), EE=EQClassesVector.end();
       EI != EE; ++EI){
    BugReportEquivClass& EQ = **EI;
    FlushReport(EQ);
  }

  // BugReporter owns and deletes only BugTypes created implicitly through
  // EmitBasicReport.
  // FIXME: There are leaks from checkers that assume that the BugTypes they
  // create will be destroyed by the BugReporter.
  for (llvm::StringMap<BugType*>::iterator
         I = StrBugTypes.begin(), E = StrBugTypes.end(); I != E; ++I)
    delete I->second;

  // Remove all references to the BugType objects.
  BugTypes = F.getEmptySet();
}

//===----------------------------------------------------------------------===//
// PathDiagnostics generation.
//===----------------------------------------------------------------------===//

static std::pair<std::pair<ExplodedGraph*, NodeBackMap*>,
                 std::pair<ExplodedNode*, unsigned> >
MakeReportGraph(const ExplodedGraph* G,
                SmallVectorImpl<const ExplodedNode*> &nodes) {

  // Create the trimmed graph.  It will contain the shortest paths from the
  // error nodes to the root.  In the new graph we should only have one
  // error node unless there are two or more error nodes with the same minimum
  // path length.
  ExplodedGraph* GTrim;
  InterExplodedGraphMap* NMap;

  llvm::DenseMap<const void*, const void*> InverseMap;
  llvm::tie(GTrim, NMap) = G->Trim(nodes.data(), nodes.data() + nodes.size(),
                                   &InverseMap);

  // Create owning pointers for GTrim and NMap just to ensure that they are
  // released when this function exists.
  OwningPtr<ExplodedGraph> AutoReleaseGTrim(GTrim);
  OwningPtr<InterExplodedGraphMap> AutoReleaseNMap(NMap);

  // Find the (first) error node in the trimmed graph.  We just need to consult
  // the node map (NMap) which maps from nodes in the original graph to nodes
  // in the new graph.

  std::queue<const ExplodedNode*> WS;
  typedef llvm::DenseMap<const ExplodedNode*, unsigned> IndexMapTy;
  IndexMapTy IndexMap;

  for (unsigned nodeIndex = 0 ; nodeIndex < nodes.size(); ++nodeIndex) {
    const ExplodedNode *originalNode = nodes[nodeIndex];
    if (const ExplodedNode *N = NMap->getMappedNode(originalNode)) {
      WS.push(N);
      IndexMap[originalNode] = nodeIndex;
    }
  }

  assert(!WS.empty() && "No error node found in the trimmed graph.");

  // Create a new (third!) graph with a single path.  This is the graph
  // that will be returned to the caller.
  ExplodedGraph *GNew = new ExplodedGraph();

  // Sometimes the trimmed graph can contain a cycle.  Perform a reverse BFS
  // to the root node, and then construct a new graph that contains only
  // a single path.
  llvm::DenseMap<const void*,unsigned> Visited;

  unsigned cnt = 0;
  const ExplodedNode *Root = 0;

  while (!WS.empty()) {
    const ExplodedNode *Node = WS.front();
    WS.pop();

    if (Visited.find(Node) != Visited.end())
      continue;

    Visited[Node] = cnt++;

    if (Node->pred_empty()) {
      Root = Node;
      break;
    }

    for (ExplodedNode::const_pred_iterator I=Node->pred_begin(),
         E=Node->pred_end(); I!=E; ++I)
      WS.push(*I);
  }

  assert(Root);

  // Now walk from the root down the BFS path, always taking the successor
  // with the lowest number.
  ExplodedNode *Last = 0, *First = 0;
  NodeBackMap *BM = new NodeBackMap();
  unsigned NodeIndex = 0;

  for ( const ExplodedNode *N = Root ;;) {
    // Lookup the number associated with the current node.
    llvm::DenseMap<const void*,unsigned>::iterator I = Visited.find(N);
    assert(I != Visited.end());

    // Create the equivalent node in the new graph with the same state
    // and location.
    ExplodedNode *NewN = GNew->getNode(N->getLocation(), N->getState());

    // Store the mapping to the original node.
    llvm::DenseMap<const void*, const void*>::iterator IMitr=InverseMap.find(N);
    assert(IMitr != InverseMap.end() && "No mapping to original node.");
    (*BM)[NewN] = (const ExplodedNode*) IMitr->second;

    // Link up the new node with the previous node.
    if (Last)
      NewN->addPredecessor(Last, *GNew);

    Last = NewN;

    // Are we at the final node?
    IndexMapTy::iterator IMI =
      IndexMap.find((const ExplodedNode*)(IMitr->second));
    if (IMI != IndexMap.end()) {
      First = NewN;
      NodeIndex = IMI->second;
      break;
    }

    // Find the next successor node.  We choose the node that is marked
    // with the lowest DFS number.
    ExplodedNode::const_succ_iterator SI = N->succ_begin();
    ExplodedNode::const_succ_iterator SE = N->succ_end();
    N = 0;

    for (unsigned MinVal = 0; SI != SE; ++SI) {

      I = Visited.find(*SI);

      if (I == Visited.end())
        continue;

      if (!N || I->second < MinVal) {
        N = *SI;
        MinVal = I->second;
      }
    }

    assert(N);
  }

  assert(First);

  return std::make_pair(std::make_pair(GNew, BM),
                        std::make_pair(First, NodeIndex));
}

/// CompactPathDiagnostic - This function postprocesses a PathDiagnostic object
///  and collapses PathDiagosticPieces that are expanded by macros.
static void CompactPathDiagnostic(PathPieces &path, const SourceManager& SM) {
  typedef std::vector<std::pair<IntrusiveRefCntPtr<PathDiagnosticMacroPiece>,
                                SourceLocation> > MacroStackTy;

  typedef std::vector<IntrusiveRefCntPtr<PathDiagnosticPiece> >
          PiecesTy;

  MacroStackTy MacroStack;
  PiecesTy Pieces;

  for (PathPieces::const_iterator I = path.begin(), E = path.end();
       I!=E; ++I) {
    
    PathDiagnosticPiece *piece = I->getPtr();

    // Recursively compact calls.
    if (PathDiagnosticCallPiece *call=dyn_cast<PathDiagnosticCallPiece>(piece)){
      CompactPathDiagnostic(call->path, SM);
    }
    
    // Get the location of the PathDiagnosticPiece.
    const FullSourceLoc Loc = piece->getLocation().asLocation();

    // Determine the instantiation location, which is the location we group
    // related PathDiagnosticPieces.
    SourceLocation InstantiationLoc = Loc.isMacroID() ?
                                      SM.getExpansionLoc(Loc) :
                                      SourceLocation();

    if (Loc.isFileID()) {
      MacroStack.clear();
      Pieces.push_back(piece);
      continue;
    }

    assert(Loc.isMacroID());

    // Is the PathDiagnosticPiece within the same macro group?
    if (!MacroStack.empty() && InstantiationLoc == MacroStack.back().second) {
      MacroStack.back().first->subPieces.push_back(piece);
      continue;
    }

    // We aren't in the same group.  Are we descending into a new macro
    // or are part of an old one?
    IntrusiveRefCntPtr<PathDiagnosticMacroPiece> MacroGroup;

    SourceLocation ParentInstantiationLoc = InstantiationLoc.isMacroID() ?
                                          SM.getExpansionLoc(Loc) :
                                          SourceLocation();

    // Walk the entire macro stack.
    while (!MacroStack.empty()) {
      if (InstantiationLoc == MacroStack.back().second) {
        MacroGroup = MacroStack.back().first;
        break;
      }

      if (ParentInstantiationLoc == MacroStack.back().second) {
        MacroGroup = MacroStack.back().first;
        break;
      }

      MacroStack.pop_back();
    }

    if (!MacroGroup || ParentInstantiationLoc == MacroStack.back().second) {
      // Create a new macro group and add it to the stack.
      PathDiagnosticMacroPiece *NewGroup =
        new PathDiagnosticMacroPiece(
          PathDiagnosticLocation::createSingleLocation(piece->getLocation()));

      if (MacroGroup)
        MacroGroup->subPieces.push_back(NewGroup);
      else {
        assert(InstantiationLoc.isFileID());
        Pieces.push_back(NewGroup);
      }

      MacroGroup = NewGroup;
      MacroStack.push_back(std::make_pair(MacroGroup, InstantiationLoc));
    }

    // Finally, add the PathDiagnosticPiece to the group.
    MacroGroup->subPieces.push_back(piece);
  }

  // Now take the pieces and construct a new PathDiagnostic.
  path.clear();

  for (PiecesTy::iterator I=Pieces.begin(), E=Pieces.end(); I!=E; ++I)
    path.push_back(*I);
}

void GRBugReporter::GeneratePathDiagnostic(PathDiagnostic& PD,
                                           PathDiagnosticConsumer &PC,
                                           ArrayRef<BugReport *> &bugReports) {

  assert(!bugReports.empty());
  SmallVector<const ExplodedNode *, 10> errorNodes;
  for (ArrayRef<BugReport*>::iterator I = bugReports.begin(),
                                      E = bugReports.end(); I != E; ++I) {
      errorNodes.push_back((*I)->getErrorNode());
  }

  // Construct a new graph that contains only a single path from the error
  // node to a root.
  const std::pair<std::pair<ExplodedGraph*, NodeBackMap*>,
  std::pair<ExplodedNode*, unsigned> >&
    GPair = MakeReportGraph(&getGraph(), errorNodes);

  // Find the BugReport with the original location.
  assert(GPair.second.second < bugReports.size());
  BugReport *R = bugReports[GPair.second.second];
  assert(R && "No original report found for sliced graph.");

  OwningPtr<ExplodedGraph> ReportGraph(GPair.first.first);
  OwningPtr<NodeBackMap> BackMap(GPair.first.second);
  const ExplodedNode *N = GPair.second.first;

  // Start building the path diagnostic...
  PathDiagnosticBuilder PDB(*this, R, BackMap.get(), &PC);

  // Register additional node visitors.
  R->addVisitor(new NilReceiverBRVisitor());
  R->addVisitor(new ConditionBRVisitor());

  BugReport::VisitorList visitors;
  unsigned originalReportConfigToken, finalReportConfigToken;

  // While generating diagnostics, it's possible the visitors will decide
  // new symbols and regions are interesting, or add other visitors based on
  // the information they find. If they do, we need to regenerate the path
  // based on our new report configuration.
  do {
    // Get a clean copy of all the visitors.
    for (BugReport::visitor_iterator I = R->visitor_begin(),
                                     E = R->visitor_end(); I != E; ++I)
       visitors.push_back((*I)->clone());

    // Clear out the active path from any previous work.
    PD.resetPath();
    originalReportConfigToken = R->getConfigurationChangeToken();

    // Generate the very last diagnostic piece - the piece is visible before 
    // the trace is expanded.
    PathDiagnosticPiece *LastPiece = 0;
    for (BugReport::visitor_iterator I = visitors.begin(), E = visitors.end();
         I != E; ++I) {
      if (PathDiagnosticPiece *Piece = (*I)->getEndPath(PDB, N, *R)) {
        assert (!LastPiece &&
                "There can only be one final piece in a diagnostic.");
        LastPiece = Piece;
      }
    }
    if (!LastPiece)
      LastPiece = BugReporterVisitor::getDefaultEndPath(PDB, N, *R);
    if (LastPiece)
      PD.setEndOfPath(LastPiece);
    else
      return;

    switch (PDB.getGenerationScheme()) {
    case PathDiagnosticConsumer::Extensive:
      GenerateExtensivePathDiagnostic(PD, PDB, N, visitors);
      break;
    case PathDiagnosticConsumer::Minimal:
      GenerateMinimalPathDiagnostic(PD, PDB, N, visitors);
      break;
    case PathDiagnosticConsumer::None:
      llvm_unreachable("PathDiagnosticConsumer::None should never appear here");
    }

    // Clean up the visitors we used.
    llvm::DeleteContainerPointers(visitors);

    // Did anything change while generating this path?
    finalReportConfigToken = R->getConfigurationChangeToken();
  } while(finalReportConfigToken != originalReportConfigToken);

  // Finally, prune the diagnostic path of uninteresting stuff.
  if (R->shouldPrunePath()) {
    bool hasSomethingInteresting = RemoveUneededCalls(PD.getMutablePieces(), R);
    assert(hasSomethingInteresting);
    (void) hasSomethingInteresting;
  }
}

void BugReporter::Register(BugType *BT) {
  BugTypes = F.add(BugTypes, BT);
}

void BugReporter::EmitReport(BugReport* R) {
  // Compute the bug report's hash to determine its equivalence class.
  llvm::FoldingSetNodeID ID;
  R->Profile(ID);

  // Lookup the equivance class.  If there isn't one, create it.
  BugType& BT = R->getBugType();
  Register(&BT);
  void *InsertPos;
  BugReportEquivClass* EQ = EQClasses.FindNodeOrInsertPos(ID, InsertPos);

  if (!EQ) {
    EQ = new BugReportEquivClass(R);
    EQClasses.InsertNode(EQ, InsertPos);
    EQClassesVector.push_back(EQ);
  }
  else
    EQ->AddReport(R);
}


//===----------------------------------------------------------------------===//
// Emitting reports in equivalence classes.
//===----------------------------------------------------------------------===//

namespace {
struct FRIEC_WLItem {
  const ExplodedNode *N;
  ExplodedNode::const_succ_iterator I, E;
  
  FRIEC_WLItem(const ExplodedNode *n)
  : N(n), I(N->succ_begin()), E(N->succ_end()) {}
};  
}

static BugReport *
FindReportInEquivalenceClass(BugReportEquivClass& EQ,
                             SmallVectorImpl<BugReport*> &bugReports) {

  BugReportEquivClass::iterator I = EQ.begin(), E = EQ.end();
  assert(I != E);
  BugType& BT = I->getBugType();

  // If we don't need to suppress any of the nodes because they are
  // post-dominated by a sink, simply add all the nodes in the equivalence class
  // to 'Nodes'.  Any of the reports will serve as a "representative" report.
  if (!BT.isSuppressOnSink()) {
    BugReport *R = I;
    for (BugReportEquivClass::iterator I=EQ.begin(), E=EQ.end(); I!=E; ++I) {
      const ExplodedNode *N = I->getErrorNode();
      if (N) {
        R = I;
        bugReports.push_back(R);
      }
    }
    return R;
  }

  // For bug reports that should be suppressed when all paths are post-dominated
  // by a sink node, iterate through the reports in the equivalence class
  // until we find one that isn't post-dominated (if one exists).  We use a
  // DFS traversal of the ExplodedGraph to find a non-sink node.  We could write
  // this as a recursive function, but we don't want to risk blowing out the
  // stack for very long paths.
  BugReport *exampleReport = 0;

  for (; I != E; ++I) {
    const ExplodedNode *errorNode = I->getErrorNode();

    if (!errorNode)
      continue;
    if (errorNode->isSink()) {
      llvm_unreachable(
           "BugType::isSuppressSink() should not be 'true' for sink end nodes");
    }
    // No successors?  By definition this nodes isn't post-dominated by a sink.
    if (errorNode->succ_empty()) {
      bugReports.push_back(I);
      if (!exampleReport)
        exampleReport = I;
      continue;
    }

    // At this point we know that 'N' is not a sink and it has at least one
    // successor.  Use a DFS worklist to find a non-sink end-of-path node.    
    typedef FRIEC_WLItem WLItem;
    typedef SmallVector<WLItem, 10> DFSWorkList;
    llvm::DenseMap<const ExplodedNode *, unsigned> Visited;
    
    DFSWorkList WL;
    WL.push_back(errorNode);
    Visited[errorNode] = 1;
    
    while (!WL.empty()) {
      WLItem &WI = WL.back();
      assert(!WI.N->succ_empty());
            
      for (; WI.I != WI.E; ++WI.I) {
        const ExplodedNode *Succ = *WI.I;        
        // End-of-path node?
        if (Succ->succ_empty()) {
          // If we found an end-of-path node that is not a sink.
          if (!Succ->isSink()) {
            bugReports.push_back(I);
            if (!exampleReport)
              exampleReport = I;
            WL.clear();
            break;
          }
          // Found a sink?  Continue on to the next successor.
          continue;
        }
        // Mark the successor as visited.  If it hasn't been explored,
        // enqueue it to the DFS worklist.
        unsigned &mark = Visited[Succ];
        if (!mark) {
          mark = 1;
          WL.push_back(Succ);
          break;
        }
      }

      // The worklist may have been cleared at this point.  First
      // check if it is empty before checking the last item.
      if (!WL.empty() && &WL.back() == &WI)
        WL.pop_back();
    }
  }

  // ExampleReport will be NULL if all the nodes in the equivalence class
  // were post-dominated by sinks.
  return exampleReport;
}

void BugReporter::FlushReport(BugReportEquivClass& EQ) {
  SmallVector<BugReport*, 10> bugReports;
  BugReport *exampleReport = FindReportInEquivalenceClass(EQ, bugReports);
  if (exampleReport) {
    const PathDiagnosticConsumers &C = getPathDiagnosticConsumers();
    for (PathDiagnosticConsumers::const_iterator I=C.begin(),
                                                 E=C.end(); I != E; ++I) {
      FlushReport(exampleReport, **I, bugReports);
    }
  }
}

void BugReporter::FlushReport(BugReport *exampleReport,
                              PathDiagnosticConsumer &PD,
                              ArrayRef<BugReport*> bugReports) {

  // FIXME: Make sure we use the 'R' for the path that was actually used.
  // Probably doesn't make a difference in practice.
  BugType& BT = exampleReport->getBugType();

  OwningPtr<PathDiagnostic>
    D(new PathDiagnostic(exampleReport->getDeclWithIssue(),
                         exampleReport->getBugType().getName(),
                         exampleReport->getDescription(),
                         exampleReport->getShortDescription(/*Fallback=*/false),
                         BT.getCategory()));

  // Generate the full path diagnostic, using the generation scheme
  // specified by the PathDiagnosticConsumer.
  if (PD.getGenerationScheme() != PathDiagnosticConsumer::None) {
    if (!bugReports.empty())
      GeneratePathDiagnostic(*D.get(), PD, bugReports);
  }

  // If the path is empty, generate a single step path with the location
  // of the issue.
  if (D->path.empty()) {
    PathDiagnosticLocation L = exampleReport->getLocation(getSourceManager());
    PathDiagnosticPiece *piece =
      new PathDiagnosticEventPiece(L, exampleReport->getDescription());
    BugReport::ranges_iterator Beg, End;
    llvm::tie(Beg, End) = exampleReport->getRanges();
    for ( ; Beg != End; ++Beg)
      piece->addRange(*Beg);
    D->setEndOfPath(piece);
  }

  // Get the meta data.
  const BugReport::ExtraTextList &Meta = exampleReport->getExtraText();
  for (BugReport::ExtraTextList::const_iterator i = Meta.begin(),
                                                e = Meta.end(); i != e; ++i) {
    D->addMeta(*i);
  }

  PD.HandlePathDiagnostic(D.take());
}

void BugReporter::EmitBasicReport(const Decl *DeclWithIssue,
                                  StringRef name,
                                  StringRef category,
                                  StringRef str, PathDiagnosticLocation Loc,
                                  SourceRange* RBeg, unsigned NumRanges) {

  // 'BT' is owned by BugReporter.
  BugType *BT = getBugTypeForName(name, category);
  BugReport *R = new BugReport(*BT, str, Loc);
  R->setDeclWithIssue(DeclWithIssue);
  for ( ; NumRanges > 0 ; --NumRanges, ++RBeg) R->addRange(*RBeg);
  EmitReport(R);
}

BugType *BugReporter::getBugTypeForName(StringRef name,
                                        StringRef category) {
  SmallString<136> fullDesc;
  llvm::raw_svector_ostream(fullDesc) << name << ":" << category;
  llvm::StringMapEntry<BugType *> &
      entry = StrBugTypes.GetOrCreateValue(fullDesc);
  BugType *BT = entry.getValue();
  if (!BT) {
    BT = new BugType(name, category);
    entry.setValue(BT);
  }
  return BT;
}
