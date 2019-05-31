/*
 * This file is part of Icecream.
 *
 * Based on LLVM/Clang.
 *
 * This file is distributed under the University of Illinois Open Source
 * License.
 *
 */

#include <clang/Basic/Version.h>
#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Frontend/FrontendPluginRegistry.h>

#include <fstream>

using namespace clang;
using namespace std;

namespace IcecreamTest
{

void report( const CompilerInstance& compiler, DiagnosticsEngine::Level level, const char* txt, SourceLocation loc = SourceLocation());

class Action
    : public PluginASTAction
    {
    public:
#if (CLANG_VERSION_MAJOR == 3 && CLANG_VERSION_MINOR >= 6) || CLANG_VERSION_MAJOR > 3
        virtual std::unique_ptr<ASTConsumer> CreateASTConsumer( CompilerInstance& compiler, StringRef infile );
#else
        virtual ASTConsumer* CreateASTConsumer( CompilerInstance& compiler, StringRef infile );
#endif
        virtual bool ParseArgs( const CompilerInstance& compiler, const vector< string >& args );
    private:
        vector< string > _args;
    };

class Consumer
    : public RecursiveASTVisitor< Consumer >, public ASTConsumer
    {
    public:
        Consumer( CompilerInstance& compiler, const vector< string >& args );
        bool VisitReturnStmt( const ReturnStmt* returnstmt );
        virtual void HandleTranslationUnit( ASTContext& context );
    private:
        CompilerInstance& compiler;
    };


#if (CLANG_VERSION_MAJOR == 3 && CLANG_VERSION_MINOR >= 6) || CLANG_VERSION_MAJOR > 3
std::unique_ptr<ASTConsumer> Action::CreateASTConsumer( CompilerInstance& compiler, StringRef )
    {
    return unique_ptr<Consumer>( new Consumer( compiler, _args ));
    }
#else
ASTConsumer* Action::CreateASTConsumer( CompilerInstance& compiler, StringRef )
    {
    return new Consumer( compiler, _args );
    }
#endif

bool Action::ParseArgs( const CompilerInstance& /*compiler*/, const vector< string >& args )
    {
    _args = args;
    return true;
    }

Consumer::Consumer( CompilerInstance& compiler, const vector< string >& args )
    : compiler( compiler )
    {
    // Check that the file passed as argument really exists (was included in ICECC_EXTRAFILES).
    if( args.size() != 1 )
        {
        report( compiler, DiagnosticsEngine::Error, "Incorrect number of arguments" );
        return;
        }
    ifstream is( args[ 0 ].c_str());
    if( !is.good())
        report( compiler, DiagnosticsEngine::Error, "Extra file open error" );
    else
        {
        char buf[ 20 ];
        is.getline( buf, 20 );
        if( strcmp( buf, "testfile" ) != 0 )
            report( compiler, DiagnosticsEngine::Error, "File contents do not match" );
        else
            report( compiler, DiagnosticsEngine::Warning, "Extra file check successful" );
        }
    }

void Consumer::HandleTranslationUnit( ASTContext& context )
    {
    if( context.getDiagnostics().hasErrorOccurred())
        return;
    TraverseDecl( compiler.getASTContext().getTranslationUnitDecl());
    }

bool Consumer::VisitReturnStmt( const ReturnStmt* returnstmt )
    {
    // Get the expression in the return statement (see ReturnStmt API docs).
    const Expr* expression = returnstmt->getRetValue();
    if( expression == NULL )
        return true; // plain 'return;' without expression
    // Check if the expression is a bool literal (Clang uses dyn_cast<> instead of dynamic_cast<>).
    if( const CXXBoolLiteralExpr* boolliteral = dyn_cast< CXXBoolLiteralExpr >( expression ))
        { // It is.
        if( boolliteral->getValue() == false )
            report( compiler, DiagnosticsEngine::Warning, "Icecream plugin found return false",
#if CLANG_VERSION_MAJOR >= 8
                returnstmt->getBeginLoc());
#else
                returnstmt->getLocStart());
#endif
        }
    return true;
    }

void report( const CompilerInstance& compiler, DiagnosticsEngine::Level level, const char* txt, SourceLocation loc )
    {
    DiagnosticsEngine& engine = compiler.getDiagnostics();
#if (CLANG_VERSION_MAJOR == 3 && CLANG_VERSION_MINOR >= 5) || CLANG_VERSION_MAJOR > 3
    if( loc.isValid())
        engine.Report( loc, engine.getDiagnosticIDs()->getCustomDiagID(
            static_cast< DiagnosticIDs::Level >( level ), txt ));
    else
        engine.Report( engine.getDiagnosticIDs()->getCustomDiagID(
            static_cast< DiagnosticIDs::Level >( level ), txt ));
#else
    if( loc.isValid())
        engine.Report( loc, engine.getCustomDiagID( level, txt ));
    else
        engine.Report( engine.getCustomDiagID( level, txt ));
#endif
    }

} // namespace

static FrontendPluginRegistry::Add< IcecreamTest::Action > X( "icecreamtest", "Icecream test plugin" );
