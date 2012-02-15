#include "RenameParam.h"

#include <sstream>

#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"

#include "TransformationManager.h"
#include "RewriteUtils.h"

using namespace clang;
using namespace llvm;

static const char *DescriptionMsg =
"Another pass to increase readability of reduced code. \
It renames function parameters to p1, p2, ...\n";

static RegisterTransformation<RenameParam>
         Trans("rename-param", DescriptionMsg);

class ExistingVarCollectionVisitor : public 
  RecursiveASTVisitor<ExistingVarCollectionVisitor> {
public:

  explicit ExistingVarCollectionVisitor(RenameParam *Instance)
    : ConsumerInstance(Instance),
      TheFunctionDecl(NULL)
  { }

  bool VisitVarDecl(VarDecl *VD);

private:

  RenameParam *ConsumerInstance;

  FunctionDecl *TheFunctionDecl;

};

class RenameParamVisitor : public RecursiveASTVisitor<RenameParamVisitor> {
public:

  explicit RenameParamVisitor(RenameParam *Instance)
    : ConsumerInstance(Instance)
  { }

  bool VisitFunctionDecl(FunctionDecl *FD);

  bool VisitDeclRefExpr(DeclRefExpr *DRE);

private:

  RenameParam *ConsumerInstance;

  DenseMap<ParmVarDecl *, std::string> ParamNameMap;

};

bool ExistingVarCollectionVisitor::VisitVarDecl(VarDecl *VD)
{
  ParmVarDecl *PD = dyn_cast<ParmVarDecl>(VD);
  if (PD) {
    ConsumerInstance->validateParam(PD);
    return true;
  }

  VarDecl *CanonicalVD = VD->getCanonicalDecl();

  if (CanonicalVD->isLocalVarDecl()) {
    ConsumerInstance->addLocalVar(VD);
  }
  else {
    ConsumerInstance->addGlobalVar(VD);
  }
  return true;
}

bool RenameParamVisitor::VisitFunctionDecl(FunctionDecl *FD)
{
  if (FD->param_size() == 0)
    return true;

  FunctionDecl *CanonicalFD = FD->getCanonicalDecl();
  unsigned int CurrPostfix = 0;
  ParamNameMap.clear();

  for(FunctionDecl::param_iterator I = FD->param_begin(),
      E = FD->param_end(); I != E; ++I) {

    CurrPostfix++;
    ParmVarDecl *PD = (*I);
    if (PD->getNameAsString().empty())
      continue;

    CurrPostfix = ConsumerInstance->validatePostfix(CanonicalFD, CurrPostfix);
    std::stringstream TmpSS;
    TmpSS << ConsumerInstance->ParamNamePrefix << CurrPostfix;

    RewriteUtils::replaceParamVarDeclName(PD, TmpSS.str(), 
           &ConsumerInstance->TheRewriter, ConsumerInstance->SrcManager);

    if (FD->isThisDeclarationADefinition()) {
      ParamNameMap[*I] = TmpSS.str();
    }
  }
  return true;
}

bool RenameParamVisitor::VisitDeclRefExpr(DeclRefExpr *DRE)
{
  ValueDecl *OrigDecl = DRE->getDecl();
  ParmVarDecl *PD = dyn_cast<ParmVarDecl>(OrigDecl);
  
  if (!PD)
    return true;

  llvm::DenseMap<ParmVarDecl *, std::string>::iterator I =
    ParamNameMap.find(PD);
  TransAssert((I != ParamNameMap.end()) && "Bad Param!");
  
  return RewriteUtils::replaceExpr(DRE, (*I).second, 
           &ConsumerInstance->TheRewriter, ConsumerInstance->SrcManager);
}

void RenameParam::Initialize(ASTContext &context) 
{
  Context = &context;
  SrcManager = &Context->getSourceManager();
  VarCollectionVisitor = new ExistingVarCollectionVisitor(this);
  RenameVisitor = new RenameParamVisitor(this);
  TheRewriter.setSourceMgr(Context->getSourceManager(), 
                           Context->getLangOptions());
}

void RenameParam::HandleTopLevelDecl(DeclGroupRef D) 
{
  for (DeclGroupRef::iterator I = D.begin(), E = D.end(); I != E; ++I) {
    VarCollectionVisitor->TraverseDecl(*I);
  }
}

void RenameParam::HandleTranslationUnit(ASTContext &Ctx)
{
  if (QueryInstanceOnly) {
    if (HasValidParams)
      ValidInstanceNum = 1;
    else
      ValidInstanceNum = 0;
    return;
  }

  if (!HasValidParams) {
    TransError = TransNoValidParamsError;
    return;
  }

  TransAssert(RenameVisitor && "NULL RenameVisitor!");
  Ctx.getDiagnostics().setSuppressAllDiagnostics(false);

  RenameVisitor->TraverseDecl(Ctx.getTranslationUnitDecl());

  if (Ctx.getDiagnostics().hasErrorOccurred() ||
      Ctx.getDiagnostics().hasFatalErrorOccurred())
    TransError = TransInternalError;
}

bool RenameParam::getPostfixValue(const std::string &Name, 
                                  unsigned int &Value)
{
  // It's an unamed parameter, we skip it
  if (Name.size() == 0)
    return true;

  if (Name.size() == 1)
    return false;

  std::string Prefix = Name.substr(0, 1);
  if (Prefix != ParamNamePrefix)
    return false;

  std::string RestStr = Name.substr(1);
  std::stringstream TmpSS(RestStr);
  if (!(TmpSS >> Value))
    return false;

  return true;
}

void RenameParam::validateParam(ParmVarDecl *PD)
{
  unsigned int Value;
  if (!getPostfixValue(PD->getNameAsString(), Value))
    HasValidParams = true;
}

void RenameParam::addGlobalVar(VarDecl *VD)
{
  unsigned int PostValue;
  if (!getPostfixValue(VD->getNameAsString(), PostValue))
    return;

  ExistingGlobalVars.insert(PostValue);
}

void RenameParam::addLocalVar(VarDecl *VD)
{
  unsigned int PostValue;
  if (!getPostfixValue(VD->getNameAsString(), PostValue))
    return;

  DeclContext *Ctx = VD->getDeclContext();
  FunctionDecl *FD = dyn_cast<FunctionDecl>(Ctx); 
  TransAssert(FD && "Bad function declaration!");
  FunctionDecl *CanonicalFD = FD->getCanonicalDecl();

  ExistingNumberSet *CurrSet;
  DenseMap<FunctionDecl *, ExistingNumberSet *>::iterator I =
    FunExistingVarsMap.find(CanonicalFD);

  if (I == FunExistingVarsMap.end()) {
    CurrSet = new ExistingNumberSet::SmallSet();
    FunExistingVarsMap[CanonicalFD] = CurrSet;
  }
  else {
    CurrSet = (*I).second;
  }

  CurrSet->insert(PostValue);
}

bool RenameParam::isValidPostfix(ExistingNumberSet *LocalSet, 
                                 unsigned int Postfix)
{
  if (ExistingGlobalVars.count(Postfix))
    return false;

  if (!LocalSet)
    return true;

  return !LocalSet->count(Postfix);
}

unsigned int RenameParam::validatePostfix(FunctionDecl *FD, 
                                          unsigned int CurrPostfix)
{
  int MaxIteration = 0;
  ExistingNumberSet *LocalNumberSet = NULL;

  llvm::DenseMap<FunctionDecl *, ExistingNumberSet *>::iterator I =
    FunExistingVarsMap.find(FD);

  if (I != FunExistingVarsMap.end()) {
    LocalNumberSet = (*I).second;
    MaxIteration += static_cast<int>(LocalNumberSet->size());
  }
  MaxIteration += static_cast<int>(ExistingGlobalVars.size());

  while (!isValidPostfix(LocalNumberSet, CurrPostfix)) {
    CurrPostfix++;
    MaxIteration--;
    TransAssert((MaxIteration >= 0) && "Bad Postfix!");
  }

  return CurrPostfix;
}

RenameParam::~RenameParam(void)
{
  if (VarCollectionVisitor)
    delete VarCollectionVisitor;
  if (RenameVisitor)
    delete RenameVisitor;

  for (DenseMap<FunctionDecl *, ExistingNumberSet *>::iterator 
        I = FunExistingVarsMap.begin(), E = FunExistingVarsMap.end();
        I != E; ++I) {
    delete (*I).second;
  }
}
