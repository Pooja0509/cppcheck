/*
 * Cppcheck - A tool for static C/C++ code analysis
 * Copyright (C) 2007-2012 Daniel Marjamäki and Cppcheck team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


//---------------------------------------------------------------------------
#include "checkuninitvar.h"
#include "mathlib.h"
#include "executionpath.h"
#include "checknullpointer.h"   // CheckNullPointer::parseFunctionCall
#include "symboldatabase.h"
#include <algorithm>
//---------------------------------------------------------------------------

// Register this check class (by creating a static instance of it)
namespace {
    CheckUninitVar instance;
}

//---------------------------------------------------------------------------


// Skip [ .. ]
static const Token * skipBrackets(const Token *tok)
{
    while (tok && tok->str() == "[")
        tok = tok->link()->next();
    return tok;
}


/// @addtogroup Checks
/// @{

/**
 * @brief %Check that uninitialized variables aren't used (using ExecutionPath)
 * */
class UninitVar : public ExecutionPath {
public:
    /** Startup constructor */
    explicit UninitVar(Check *c, const SymbolDatabase* db, bool isc)
        : ExecutionPath(c, 0), symbolDatabase(db), isC(isc), var(0), alloc(false), strncpy_(false), memset_nonzero(false) {
    }

private:
    /** Create a copy of this check */
    ExecutionPath *copy() {
        return new UninitVar(*this);
    }

    /** no implementation => compiler error if used */
    void operator=(const UninitVar &);

    /** internal constructor for creating extra checks */
    UninitVar(Check *c, const Variable* v, const SymbolDatabase* db, bool isc)
        : ExecutionPath(c, v->varId()), symbolDatabase(db), isC(isc), var(v), alloc(false), strncpy_(false), memset_nonzero(false) {
    }

    /** is other execution path equal? */
    bool is_equal(const ExecutionPath *e) const {
        const UninitVar *c = static_cast<const UninitVar *>(e);
        return (var == c->var && alloc == c->alloc && strncpy_ == c->strncpy_ && memset_nonzero == c->memset_nonzero);
    }

    /** pointer to symbol database */
    const SymbolDatabase* symbolDatabase;

    const bool isC;

    /** variable for this check */
    const Variable* var;

    /** is this variable allocated? */
    bool  alloc;

    /** is this variable initialized with strncpy (not always zero-terminated) */
    bool  strncpy_;

    /** is this variable initialized but not zero-terminated (memset) */
    bool  memset_nonzero;

    /** allocating pointer. For example : p = malloc(10); */
    static void alloc_pointer(std::list<ExecutionPath *> &checks, unsigned int varid) {
        // loop through the checks and perform a allocation if the
        // variable id matches
        std::list<ExecutionPath *>::const_iterator it;
        for (it = checks.begin(); it != checks.end(); ++it) {
            UninitVar *c = dynamic_cast<UninitVar *>(*it);
            if (c && c->varId == varid) {
                if (c->var->isPointer() && !c->var->isArray())
                    c->alloc = true;
                else
                    bailOutVar(checks, varid);
                break;
            }
        }
    }

    /** Initializing a pointer value. For example: *p = 0; */
    static void init_pointer(std::list<ExecutionPath *> &checks, const Token *tok) {
        const unsigned int varid(tok->varId());
        if (!varid)
            return;

        // loop through the checks and perform a initialization if the
        // variable id matches
        std::list<ExecutionPath *>::iterator it = checks.begin();
        while (it != checks.end()) {
            UninitVar *c = dynamic_cast<UninitVar *>(*it);
            if (c && c->varId == varid) {
                if (c->alloc || c->var->isArray()) {
                    delete c;
                    checks.erase(it++);
                    continue;
                } else {
                    use_pointer(checks, tok);
                }
            }

            ++it;
        }
    }

    /** Deallocate a pointer. For example: free(p); */
    static void dealloc_pointer(std::list<ExecutionPath *> &checks, const Token *tok) {
        const unsigned int varid(tok->varId());
        if (!varid)
            return;

        // loop through the checks and perform a deallocation if the
        // variable id matches
        std::list<ExecutionPath *>::const_iterator it;
        for (it = checks.begin(); it != checks.end(); ++it) {
            UninitVar *c = dynamic_cast<UninitVar *>(*it);
            if (c && c->varId == varid) {
                // unallocated pointer variable => error
                if (c->var->isPointer() && !c->var->isArray() && !c->alloc) {
                    CheckUninitVar *checkUninitVar = dynamic_cast<CheckUninitVar *>(c->owner);
                    if (checkUninitVar) {
                        checkUninitVar->uninitvarError(tok, c->var->name());
                        break;
                    }
                }
                c->alloc = false;
            }
        }
    }

    /**
     * Pointer assignment:  p = x;
     * if p is a pointer and x is an array/pointer then bail out
     * \param checks all available checks
     * \param tok1 the "p" token
     * \param tok2 the "x" token
     */
    static void pointer_assignment(std::list<ExecutionPath *> &checks, const Token *tok1, const Token *tok2) {
        // Variable id for "left hand side" variable
        const unsigned int varid1(tok1->varId());
        if (varid1 == 0)
            return;

        // Variable id for "right hand side" variable
        const unsigned int varid2(tok2->varId());
        if (varid2 == 0)
            return;

        std::list<ExecutionPath *>::const_iterator it;

        // bail out if first variable is a pointer
        for (it = checks.begin(); it != checks.end(); ++it) {
            UninitVar *c = dynamic_cast<UninitVar *>(*it);
            if (c && c->varId == varid1 && c->var->isPointer() && !c->var->isArray()) {
                bailOutVar(checks, varid1);
                break;
            }
        }

        // bail out if second variable is a array/pointer
        for (it = checks.begin(); it != checks.end(); ++it) {
            UninitVar *c = dynamic_cast<UninitVar *>(*it);
            if (c && c->varId == varid2 && (c->var->isPointer() || c->var->isArray())) {
                bailOutVar(checks, varid2);
                break;
            }
        }
    }


    /** Initialize an array with strncpy. */
    static void init_strncpy(std::list<ExecutionPath *> &checks, const Token *tok) {
        const unsigned int varid(tok->varId());
        if (!varid)
            return;

        std::list<ExecutionPath *>::const_iterator it;
        for (it = checks.begin(); it != checks.end(); ++it) {
            UninitVar *c = dynamic_cast<UninitVar *>(*it);
            if (c && c->varId == varid) {
                c->strncpy_ = true;
            }
        }
    }

    /** Initialize an array with memset (not zero). */
    static void init_memset_nonzero(std::list<ExecutionPath *> &checks, const Token *tok) {
        const unsigned int varid(tok->varId());
        if (!varid)
            return;

        std::list<ExecutionPath *>::const_iterator it;
        for (it = checks.begin(); it != checks.end(); ++it) {
            UninitVar *c = dynamic_cast<UninitVar *>(*it);
            if (c && c->varId == varid) {
                c->memset_nonzero = true;
            }
        }
    }



    /**
     * use - called from the use* functions below.
     * @param checks all available checks
     * @param tok variable token
     * @param mode specific behaviour
     * @return if error is found, true is returned
     */
    static bool use(std::list<ExecutionPath *> &checks, const Token *tok, const int mode) {
        const unsigned int varid(tok->varId());
        if (varid == 0)
            return false;

        std::list<ExecutionPath *>::const_iterator it;
        for (it = checks.begin(); it != checks.end(); ++it) {
            UninitVar *c = dynamic_cast<UninitVar *>(*it);
            if (c && c->varId == varid) {
                // mode 0 : the variable is used "directly"
                // example: .. = var;
                // it is ok to read the address of an uninitialized array.
                // it is ok to read the address of an allocated pointer
                if (mode == 0 && (c->var->isArray() || (c->var->isPointer() && c->alloc)))
                    continue;

                // mode 2 : reading array data with mem.. function. It's ok if the
                //          array is not null-terminated
                if (mode == 2 && c->strncpy_)
                    continue;

                // mode 3 : bad usage of pointer. if it's not a pointer then the usage is ok.
                // example: ptr->foo();
                if (mode == 3 && (!c->var->isPointer() || c->var->isArray()))
                    continue;

                // mode 4 : using dead pointer is invalid.
                if (mode == 4 && (!c->var->isPointer() || c->var->isArray() || c->alloc))
                    continue;

                // mode 5 : reading uninitialized array or pointer is invalid.
                if (mode == 5 && (!c->var->isArray() && !c->var->isPointer()))
                    continue;

                CheckUninitVar *checkUninitVar = dynamic_cast<CheckUninitVar *>(c->owner);
                if (checkUninitVar) {
                    if (c->strncpy_ || c->memset_nonzero)
                        checkUninitVar->uninitstringError(tok, c->var->name(), c->strncpy_);
                    else if (c->var->isPointer() && !c->var->isArray() && c->alloc)
                        checkUninitVar->uninitdataError(tok, c->var->name());
                    else
                        checkUninitVar->uninitvarError(tok, c->var->name());
                    return true;
                }
            }
        }

        // No error found
        return false;
    }

    /**
     * Reading variable. Use this function in situations when it is
     * invalid to read the data of the variable but not the address.
     * @param checks all available checks
     * @param tok variable token
     * @return if error is found, true is returned
     */
    static bool use(std::list<ExecutionPath *> &checks, const Token *tok) {
        return use(checks, tok, 0);
    }

    /**
     * Reading array elements. If the variable is not an array then the usage is ok.
     * @param checks all available checks
     * @param tok variable token
     */
    static void use_array(std::list<ExecutionPath *> &checks, const Token *tok) {
        use(checks, tok, 1);
    }

    /**
     * Reading array elements with a "mem.." function. It's ok if the array is not null-terminated.
     * @param checks all available checks
     * @param tok variable token
     */
    static void use_array_mem(std::list<ExecutionPath *> &checks, const Token *tok) {
        use(checks, tok, 2);
    }

    /**
     * Bad pointer usage. If the variable is not a pointer then the usage is ok.
     * @param checks all available checks
     * @param tok variable token
     * @return if error is found, true is returned
     */
    static bool use_pointer(std::list<ExecutionPath *> &checks, const Token *tok) {
        return use(checks, tok, 3);
    }

    /**
     * Using variable.. if it's a dead pointer the usage is invalid.
     * @param checks all available checks
     * @param tok variable token
     * @return if error is found, true is returned
     */
    static bool use_dead_pointer(std::list<ExecutionPath *> &checks, const Token *tok) {
        return use(checks, tok, 4);
    }

    /**
     * Using variable.. reading from uninitialized array or pointer data is invalid.
     * Example: = x[0];
     * @param checks all available checks
     * @param tok variable token
     * @return if error is found, true is returned
     */
    static bool use_array_or_pointer_data(std::list<ExecutionPath *> &checks, const Token *tok) {
        return use(checks, tok, 5);
    }

    /**
     * Parse right hand side expression in statement
     * @param tok2 start token of rhs
     * @param checks the execution paths
     */
    void parserhs(const Token *tok2, std::list<ExecutionPath *> &checks) const {
        // check variable usages in rhs/index
        while (NULL != (tok2 = tok2->next())) {
            if (Token::Match(tok2, "[;)=]"))
                break;
            if (Token::Match(tok2, "%var% ("))
                break;
            if (tok2->varId() &&
                !Token::Match(tok2->previous(), "&|::") &&
                !Token::simpleMatch(tok2->tokAt(-2), "& (") &&
                !Token::simpleMatch(tok2->next(), "=")) {
                // Multiple assignments..
                if (Token::Match(tok2->next(), ".|[")) {
                    const Token * tok3 = tok2;
                    while (tok3) {
                        if (Token::Match(tok3->next(), ". %var%"))
                            tok3 = tok3->tokAt(2);
                        else if (tok3->strAt(1) == "[")
                            tok3 = tok3->next()->link();
                        else
                            break;
                    }
                    if (tok3 && tok3->strAt(1) == "=")
                        continue;
                }
                bool foundError;
                if (tok2->previous()->str() == "*" || tok2->next()->str() == "[")
                    foundError = use_array_or_pointer_data(checks, tok2);
                else
                    foundError = use(checks, tok2);

                // prevent duplicate error messages
                if (foundError) {
                    bailOutVar(checks, tok2->varId());
                }
            }
        }

    }

    /** parse tokens. @sa ExecutionPath::parse */
    const Token *parse(const Token &tok, std::list<ExecutionPath *> &checks) const {
        // Variable declaration..
        if (tok.varId() && Token::Match(&tok, "%var% [[;]")) {
            const Variable* var2 = symbolDatabase->getVariableFromVarId(tok.varId());
            if (var2 && var2->nameToken() == &tok && !var2->isStatic() && !var2->isExtern() && !var2->isConst()) {
                if (tok.linkAt(1)) { // array
                    const Token* endtok = tok.next();
                    while (endtok->link())
                        endtok = endtok->link()->next();
                    if (endtok->str() != ";")
                        return &tok;
                }
                const Scope* parent = var2->scope()->nestedIn;
                while (parent) {
                    for (std::list<Variable>::const_iterator j = parent->varlist.begin(); j != parent->varlist.end(); ++j) {
                        if (j->name() == var2->name()) {
                            ExecutionPath::bailOutVar(checks, j->varId()); // If there is a variable with the same name in other scopes, this might cause false positives, if there are unexpanded macros
                            break;
                        }
                    }
                    parent = parent->nestedIn;
                }

                if (var2->isPointer())
                    checks.push_back(new UninitVar(owner, var2, symbolDatabase, isC));
                else if (var2->typeEndToken()->str() != ">") {
                    bool stdtype = false;  // TODO: change to isC to handle unknown types better
                    for (const Token* tok2 = var2->typeStartToken(); tok2 != var2->nameToken(); tok2 = tok2->next()) {
                        if (tok2->isStandardType()) {
                            stdtype = true;
                            break;
                        }
                    }
                    if (stdtype && (!var2->isArray() || var2->nameToken()->linkAt(1)->strAt(1) == ";"))
                        checks.push_back(new UninitVar(owner, var2, symbolDatabase, isC));
                }
                return &tok;
            }
        }

        if (tok.str() == "return") {
            // is there assignment or ternary operator in the return statement?
            bool assignment = false;
            for (const Token *tok2 = tok.next(); tok2 && tok2->str() != ";"; tok2 = tok2->next()) {
                if (tok2->str() == "=" || (!isC && tok2->str() == ">>") || Token::Match(tok2, "(|, &")) {
                    assignment = true;
                    break;
                }
                if (Token::Match(tok2, "[(,] &| %var% [,)]")) {
                    tok2 = tok2->next();
                    if (!tok2->isName())
                        tok2 = tok2->next();
                    ExecutionPath::bailOutVar(checks, tok2->varId());
                }
            }

            if (!assignment) {
                for (const Token *tok2 = tok.next(); tok2 && tok2->str() != ";"; tok2 = tok2->next()) {
                    if (tok2->isName() && tok2->strAt(1) == "(")
                        tok2 = tok2->next()->link();

                    else if (tok2->varId())
                        use(checks, tok2);
                }
            }
        }

        if (tok.varId()) {
            // array variable passed as function parameter..
            if (Token::Match(tok.previous(), "[(,] %var% [+-,)]")) {
                // skip ')'..
                const Token *tok2 = tok.next();
                while (tok2 && tok2->str() == ")")
                    tok2 = tok2->next();

                // variable is assigned like: "( %var% ) .. ="
                if (Token::Match(tok.previous(), "( %var% )") && tok2 && tok2->str() == "=")
                    ExecutionPath::bailOutVar(checks, tok.varId());
                else if (tok.strAt(-2) != ">" || !tok.linkAt(-2))
                    use(checks, &tok);
                //use_array(checks, &tok);
                return &tok;
            }

            // Used..
            if (Token::Match(tok.previous(), "[[(,+-*/|=] %var% ]|)|,|;|%op%")) {
                // Taking address of array..
                std::list<ExecutionPath *>::const_iterator it;
                for (it = checks.begin(); it != checks.end(); ++it) {
                    UninitVar *c = dynamic_cast<UninitVar *>(*it);
                    if (c && c->varId == tok.varId()) {
                        if (c->var->isArray())
                            bailOutVar(checks, tok.varId());
                        break;
                    }
                }

                // initialize reference variable
                if (Token::Match(tok.tokAt(-3), "& %var% ="))
                    bailOutVar(checks, tok.varId());
                else
                    use(checks, &tok);
                return &tok;
            }

            if ((tok.previous() && tok.previous()->type() == Token::eIncDecOp) || (tok.next() && tok.next()->type() == Token::eIncDecOp)) {
                use(checks, &tok);
                return &tok;
            }

            if (Token::Match(tok.previous(), "[;{}] %var% [=[.]")) {
                if (tok.next()->str() == ".") {
                    if (use_dead_pointer(checks, &tok)) {
                        return &tok;
                    }
                } else {
                    const Token *tok2 = tok.next();

                    if (tok2->str() == "[") {
                        const Token *tok3 = tok2->link();
                        while (Token::simpleMatch(tok3, "] ["))
                            tok3 = tok3->next()->link();

                        // Possible initialization
                        if (Token::simpleMatch(tok3, "] >>"))
                            return &tok;

                        if (Token::simpleMatch(tok3, "] =")) {
                            if (use_dead_pointer(checks, &tok)) {
                                return &tok;
                            }

                            parserhs(tok2, checks);
                            tok2 = tok3->next();
                        }
                    }

                    parserhs(tok2, checks);
                }

                // pointer aliasing?
                if (Token::Match(tok.tokAt(2), "%var% ;")) {
                    pointer_assignment(checks, &tok, tok.tokAt(2));
                }
            }

            if (Token::simpleMatch(tok.next(), "(")) {
                use_pointer(checks, &tok);
            }

            if (Token::Match(tok.tokAt(-2), "[;{}] *")) {
                if (Token::simpleMatch(tok.next(), "=")) {
                    // is the pointer used in the rhs?
                    bool used = false;
                    for (const Token *tok2 = tok.tokAt(2); tok2; tok2 = tok2->next()) {
                        if (Token::Match(tok2, "[,;=(]"))
                            break;
                        else if (Token::Match(tok2, "* %varid%", tok.varId())) {
                            used = true;
                            break;
                        }
                    }
                    if (used)
                        use_pointer(checks, &tok);
                    else
                        init_pointer(checks, &tok);
                } else {
                    use_pointer(checks, &tok);
                }
                return &tok;
            }

            if (Token::Match(tok.next(), "= malloc|kmalloc") || Token::simpleMatch(tok.next(), "= new char [")) {
                alloc_pointer(checks, tok.varId());
                if (tok.strAt(3) == "(")
                    return tok.tokAt(3);
            }

            else if ((!isC && Token::Match(tok.previous(), "<<|>>")) || Token::simpleMatch(tok.next(), "=")) {
                // TODO: Don't bail out for "<<" and ">>" if these are
                // just computations
                ExecutionPath::bailOutVar(checks, tok.varId());
                return &tok;
            }

            if (Token::simpleMatch(tok.next(), "[")) {
                const Token *tok2 = tok.next()->link();
                if (Token::simpleMatch(tok2 ? tok2->next() : 0, "=")) {
                    ExecutionPath::bailOutVar(checks, tok.varId());
                    return &tok;
                }
            }

            if (Token::simpleMatch(tok.previous(), "delete") ||
                Token::simpleMatch(tok.tokAt(-3), "delete [ ]")) {
                dealloc_pointer(checks, &tok);
                return &tok;
            }
        }

        if (Token::Match(&tok, "%var% (") && uvarFunctions.find(tok.str()) == uvarFunctions.end()) {
            // sizeof/typeof doesn't dereference. A function name that is all uppercase
            // might be an unexpanded macro that uses sizeof/typeof
            if (Token::Match(&tok, "sizeof|typeof ("))
                return tok.next()->link();

            // deallocate pointer
            if (Token::Match(&tok, "free|kfree|fclose ( %var% )") ||
                Token::Match(&tok, "realloc ( %var%")) {
                dealloc_pointer(checks, tok.tokAt(2));
                return tok.tokAt(3);
            }

            // parse usage..
            {
                std::list<const Token *> var1;
                CheckNullPointer::parseFunctionCall(tok, var1, 1);
                for (std::list<const Token *>::const_iterator it = var1.begin(); it != var1.end(); ++it) {
                    // does iterator point at first function parameter?
                    const bool firstPar(*it == tok.tokAt(2));

                    // is function memset/memcpy/etc?
                    if (tok.str().compare(0,3,"mem") == 0)
                        use_array_mem(checks, *it);

                    // second parameter for strncpy/strncat/etc
                    else if (!firstPar && tok.str().compare(0,4,"strn") == 0)
                        use_array_mem(checks, *it);

                    else
                        use_array(checks, *it);

                    use_dead_pointer(checks, *it);
                }

                // Using uninitialized pointer is bad if using null pointer is bad
                std::list<const Token *> var2;
                CheckNullPointer::parseFunctionCall(tok, var2, 0);
                for (std::list<const Token *>::const_iterator it = var2.begin(); it != var2.end(); ++it) {
                    if (std::find(var1.begin(), var1.end(), *it) == var1.end())
                        use_dead_pointer(checks, *it);
                }
            }

            // strncpy doesn't null-terminate first parameter
            if (Token::Match(&tok, "strncpy ( %var% ,")) {
                if (Token::Match(tok.tokAt(4), "%str% ,")) {
                    if (Token::Match(tok.tokAt(6), "%num% )")) {
                        const std::size_t len = Token::getStrLength(tok.tokAt(4));
                        const MathLib::bigint sz = MathLib::toLongNumber(tok.strAt(6));
                        if (sz >= 0 && len >= static_cast<unsigned long>(sz)) {
                            init_strncpy(checks, tok.tokAt(2));
                            return tok.next()->link();
                        }
                    }
                } else {
                    init_strncpy(checks, tok.tokAt(2));
                    return tok.next()->link();
                }
            }

            // memset (not zero terminated)..
            if (Token::Match(&tok, "memset ( %var% , !!0 , %num% )")) {
                init_memset_nonzero(checks, tok.tokAt(2));
                return tok.next()->link();
            }

            if (Token::Match(&tok, "asm ( %str% )")) {
                ExecutionPath::bailOut(checks);
                return &tok;
            }

            // is the variable passed as a parameter to some function?
            unsigned int parlevel = 0;
            std::set<unsigned int> bailouts;
            for (const Token *tok2 = tok.next(); tok2; tok2 = tok2->next()) {
                if (tok2->str() == "(")
                    ++parlevel;

                else if (tok2->str() == ")") {
                    if (parlevel <= 1)
                        break;
                    --parlevel;
                }

                else if (Token::Match(tok2, "sizeof|typeof (")) {
                    tok2 = tok2->next()->link();
                    if (!tok2)
                        break;
                }

                // ticket #2367 : unexpanded macro that uses sizeof|typeof?
                else if (Token::Match(tok2, "%type% (") && tok2->isUpperCaseName()) {
                    tok2 = tok2->next()->link();
                    if (!tok2)
                        break;
                }

                else if (tok2->varId()) {
                    if (Token::Match(tok2->tokAt(-2), "[(,] *") || Token::Match(tok2->next(), ". %var%")) {
                        // find function call..
                        const Token *functionCall = tok2;
                        while (NULL != (functionCall = functionCall ? functionCall->previous() : 0)) {
                            if (functionCall->str() == "(")
                                break;
                            if (functionCall->str() == ")")
                                functionCall = functionCall->link();
                        }

                        functionCall = functionCall ? functionCall->previous() : 0;
                        if (functionCall) {
                            if (functionCall->isName() && !functionCall->isUpperCaseName() && use_dead_pointer(checks, tok2))
                                ExecutionPath::bailOutVar(checks, tok2->varId());
                        }
                    }

                    // it is possible that the variable is initialized here
                    if (Token::Match(tok2->previous(), "[(,] %var% [,)]"))
                        bailouts.insert(tok2->varId());

                    // array initialization..
                    if (Token::Match(tok2->previous(), "[,(] %var% [+-]")) {
                        // if var is array, bailout
                        for (std::list<ExecutionPath *>::const_iterator it = checks.begin(); it != checks.end(); ++it) {
                            if ((*it)->varId == tok2->varId()) {
                                const UninitVar *c = dynamic_cast<const UninitVar *>(*it);
                                if (c && (c->var->isArray() || (c->var->isPointer() && c->alloc)))
                                    bailouts.insert(tok2->varId());
                                break;
                            }
                        }
                    }
                }
            }

            for (std::set<unsigned int>::const_iterator it = bailouts.begin(); it != bailouts.end(); ++it)
                ExecutionPath::bailOutVar(checks, *it);
        }

        // function call via function pointer
        if (Token::Match(&tok, "( * %var% ) (") ||
            (Token::Match(&tok, "( *| %var% .|::") && Token::Match(tok.link()->tokAt(-2), ".|:: %var% ) ("))) {
            // is the variable passed as a parameter to some function?
            const Token *tok2 = tok.link()->next();
            for (const Token* const end2 = tok2->link(); tok2 != end2; tok2 = tok2->next()) {
                if (tok2->varId()) {
                    // it is possible that the variable is initialized here
                    ExecutionPath::bailOutVar(checks, tok2->varId());
                }
            }
        }

        if (tok.str() == "return") {
            // Todo: if (!array && ..
            if (Token::Match(tok.next(), "%var% ;")) {
                use(checks, tok.next());
            } else if (Token::Match(tok.next(), "%var% [")) {
                use_array_or_pointer_data(checks, tok.next());
            }
        }

        if (tok.varId()) {
            if (Token::simpleMatch(tok.previous(), "=")) {
                if (Token::Match(tok.tokAt(-3), "& %var% =")) {
                    bailOutVar(checks, tok.varId());
                    return &tok;
                }

                if (!Token::Match(tok.tokAt(-3), ". %var% =")) {
                    if (!Token::Match(tok.tokAt(-3), "[;{}] %var% =")) {
                        use(checks, &tok);
                        return &tok;
                    }

                    const unsigned int varid2 = tok.tokAt(-2)->varId();
                    if (varid2) {
                        {
                            use(checks, &tok);
                            return &tok;
                        }
                    }
                }
            }

            if (Token::simpleMatch(tok.next(), ".")) {
                bailOutVar(checks, tok.varId());
                return &tok;
            }

            if (Token::simpleMatch(tok.next(), "[")) {
                ExecutionPath::bailOutVar(checks, tok.varId());
                return &tok;
            }

            if (Token::Match(tok.tokAt(-2), "[,(=] *")) {
                use_pointer(checks, &tok);
                return &tok;
            }

            if (Token::simpleMatch(tok.previous(), "&")) {
                ExecutionPath::bailOutVar(checks, tok.varId());
            }
        }

        // Parse "for"
        if (Token::Match(&tok, "[;{}] for (")) {
            // initialized variables
            std::set<unsigned int> varid1;
            varid1.insert(0);

            // Parse token
            const Token *tok2;

            // parse setup
            for (tok2 = tok.tokAt(3); tok2 != tok.link(); tok2 = tok2->next()) {
                if (tok2->str() == ";")
                    break;
                if (tok2->varId())
                    varid1.insert(tok2->varId());
            }
            if (tok2 == tok.link())
                return &tok;

            // parse condition
            if (Token::Match(tok2, "; %var% <|<=|>=|> %num% ;")) {
                // If the variable hasn't been initialized then call "use"
                if (varid1.find(tok2->next()->varId()) == varid1.end())
                    use(checks, tok2->next());
            }

            // goto stepcode
            tok2 = tok2->next();
            while (tok2 && tok2->str() != ";")
                tok2 = tok2->next();

            // parse the stepcode
            if (Token::Match(tok2, "; ++|-- %var% ) {") ||
                Token::Match(tok2, "; %var% ++|-- ) {")) {
                // get id of variable..
                unsigned int varid = tok2->next()->varId();
                if (!varid)
                    varid = tok2->tokAt(2)->varId();

                // Check that the variable hasn't been initialized and
                // that it isn't initialized in the body..
                if (varid1.find(varid) == varid1.end()) {
                    for (const Token *tok3 = tok2->tokAt(5); tok3 && tok3 != tok2->linkAt(4); tok3 = tok3->next()) {
                        if (tok3->varId() == varid) {
                            varid = 0;  // variable is used.. maybe it's initialized. clear the variable id.
                            break;
                        }
                    }

                    // If the variable isn't initialized in the body call "use"
                    if (varid != 0) {
                        // goto variable
                        tok2 = tok2->next();
                        if (!tok2->varId())
                            tok2 = tok2->next();

                        // call "use"
                        use(checks, tok2);
                    }
                }
            }
        }

        return &tok;
    }

    bool parseCondition(const Token &tok, std::list<ExecutionPath *> &checks) {
        if (tok.varId() && Token::Match(&tok, "%var% <|<=|==|!=|)"))
            use(checks, &tok);

        else if (Token::Match(&tok, "!| %var% [") && !Token::simpleMatch(skipBrackets(tok.next()), "="))
            use_array_or_pointer_data(checks, tok.str() == "!" ? tok.next() : &tok);

        else if (Token::Match(&tok, "!| %var% (")) {
            const Token * const ftok = (tok.str() == "!") ? tok.next() : &tok;
            std::list<const Token *> var1;
            CheckNullPointer::parseFunctionCall(*ftok, var1, 1);
            for (std::list<const Token *>::const_iterator it = var1.begin(); it != var1.end(); ++it) {
                // is function memset/memcpy/etc?
                if (ftok->str().compare(0,3,"mem") == 0)
                    use_array_mem(checks, *it);
                else
                    use_array(checks, *it);
            }
        }

        else if (Token::Match(&tok, "! %var% )")) {
            use(checks, &tok);
            return false;
        }

        return ExecutionPath::parseCondition(tok, checks);
    }

    void parseLoopBody(const Token *tok, std::list<ExecutionPath *> &checks) const {
        while (tok) {
            if (tok->str() == "{" || tok->str() == "}" || tok->str() == "for")
                return;
            if (Token::simpleMatch(tok, "if (")) {
                // bail out all variables that are used in the condition
                const Token* const end2 = tok->linkAt(1);
                for (const Token *tok2 = tok->tokAt(2); tok2 != end2; tok2 = tok2->next()) {
                    if (tok2->varId())
                        ExecutionPath::bailOutVar(checks, tok2->varId());
                }
            }
            const Token *next = parse(*tok, checks);
            tok = next->next();
        }
    }

public:

    /** Functions that don't handle uninitialized variables well */
    static std::set<std::string> uvarFunctions;

    static void analyseFunctions(const Token * const tokens, std::set<std::string> &func) {
        for (const Token *tok = tokens; tok; tok = tok->next()) {
            if (tok->str() == "{") {
                tok = tok->link();
                continue;
            }
            if (tok->str() != "::" && Token::Match(tok->next(), "%var% ( %type%")) {
                if (!Token::Match(tok->linkAt(2), ") [{;]"))
                    continue;
                const Token *tok2 = tok->tokAt(3);
                while (tok2 && tok2->str() != ")") {
                    if (tok2->str() == ",")
                        tok2 = tok2->next();

                    if (Token::Match(tok2, "%type% %var% ,|)") && tok2->isStandardType()) {
                        tok2 = tok2->tokAt(2);
                        continue;
                    }

                    if (tok2->isStandardType() && Token::Match(tok2, "%type% & %var% ,|)")) {
                        const unsigned int varid(tok2->tokAt(2)->varId());

                        // flags for read/write
                        bool r = false, w = false;

                        // check how the variable is used in the function
                        unsigned int indentlevel = 0;
                        for (const Token *tok3 = tok2; tok3; tok3 = tok3->next()) {
                            if (tok3->str() == "{")
                                ++indentlevel;
                            else if (tok3->str() == "}") {
                                if (indentlevel <= 1)
                                    break;
                                --indentlevel;
                            } else if (indentlevel == 0 && tok3->str() == ";")
                                break;
                            else if (indentlevel >= 1 && tok3->varId() == varid) {
                                if (tok3->previous()->type() == Token::eIncDecOp ||
                                    tok3->next()->type() == Token::eIncDecOp) {
                                    r = true;
                                }

                                else {
                                    w = true;
                                    break;
                                }
                            }
                        }

                        if (!r || w)
                            break;

                        tok2 = tok2->tokAt(3);
                        continue;
                    }

                    if (Token::Match(tok2, "const %type% &|*| const| %var% ,|)") && tok2->next()->isStandardType()) {
                        tok2 = tok2->tokAt(3);
                        while (tok2->isName())
                            tok2 = tok2->next();
                        continue;
                    }

                    if (Token::Match(tok2, "const %type% %var% [ ] ,|)") && tok2->next()->isStandardType()) {
                        tok2 = tok2->tokAt(5);
                        continue;
                    }

                    /// @todo enable this code. if pointer is written in function then dead pointer is invalid but valid pointer is ok.
                    /*
                    if (Token::Match(tok2, "const| struct| %type% * %var% ,|)"))
                    {
                        while (tok2->isName())
                            tok2 = tok2->next();
                        tok2 = tok2->tokAt(2);
                        continue;
                    }
                    */

                    break;
                }

                // found simple function..
                if (tok2 && tok2->link() == tok->tokAt(2))
                    func.insert(tok->next()->str());
            }
        }
    }
};

/** Functions that don't handle uninitialized variables well */
std::set<std::string> UninitVar::uvarFunctions;


/// @}


void CheckUninitVar::analyse(const Token * const tokens, std::set<std::string> &func) const
{
    UninitVar::analyseFunctions(tokens, func);
}

void CheckUninitVar::saveAnalysisData(const std::set<std::string> &data) const
{
    UninitVar::uvarFunctions.insert(data.begin(), data.end());
}

void CheckUninitVar::executionPaths()
{
    // check if variable is accessed uninitialized..
    {
        // no writing if multiple threads are used (TODO: thread safe analysis?)
        if (_settings->_jobs == 1)
            UninitVar::analyseFunctions(_tokenizer->tokens(), UninitVar::uvarFunctions);

        UninitVar c(this, _tokenizer->getSymbolDatabase(), _tokenizer->isC());
        checkExecutionPaths(_tokenizer->getSymbolDatabase(), &c);
    }
}


void CheckUninitVar::check()
{
    const SymbolDatabase *symbolDatabase = _tokenizer->getSymbolDatabase();
    std::list<Scope>::const_iterator func_scope;

    // scan every function
    for (func_scope = symbolDatabase->scopeList.begin(); func_scope != symbolDatabase->scopeList.end(); ++func_scope) {
        // only check functions
        if (func_scope->type == Scope::eFunction) {
            checkScope(&*func_scope);
        }
    }
}

void CheckUninitVar::checkScope(const Scope* scope)
{
    for (std::list<Variable>::const_iterator i = scope->varlist.begin(); i != scope->varlist.end(); ++i) {
        if ((i->type() && !i->isPointer()) || i->isStatic() || i->isExtern() || i->isConst() || i->isArray() || i->isReference())
            continue;
        if (i->nameToken()->strAt(1) == "(")
            continue;

        bool forHead = false; // Don't check variables declared in header of a for loop
        for (const Token* tok = i->typeStartToken(); tok; tok = tok->previous()) {
            if (tok->str() == "(") {
                forHead = true;
                break;
            } else if (tok->str() == "{" || tok->str() == ";" || tok->str() == "}")
                break;
        }
        if (forHead)
            continue;

        bool stdtype = _tokenizer->isC();
        const Token* tok = i->typeStartToken();
        for (; tok && tok->str() != ";" && tok->str() != "<"; tok = tok->next()) {
            if (tok->isStandardType())
                stdtype = true;
        }
        while (tok && tok->str() != ";")
            tok = tok->next();
        if (stdtype || i->isPointer())
            checkScopeForVariable(tok, *i, NULL);
    }

    for (std::list<Scope*>::const_iterator i = scope->nestedList.begin(); i != scope->nestedList.end(); ++i) {
        if (!(*i)->isClassOrStruct())
            checkScope(*i);
    }
}

bool CheckUninitVar::checkScopeForVariable(const Token *tok, const Variable& var, bool * const possibleInit)
{
    const bool suppressErrors(possibleInit && *possibleInit);

    if (possibleInit)
        *possibleInit = false;

    bool ret = false;

    unsigned int number_of_if = 0;

    // variables that are known to be non-zero
    std::set<unsigned int> notzero;

    for (; tok; tok = tok->next()) {
        // End of scope..
        if (tok->str() == "}") {
            if (number_of_if && possibleInit)
                *possibleInit = true;

            // might be a noreturn function..
            if (_tokenizer->IsScopeNoReturn(tok))
                return true;

            break;
        }

        // Unconditional inner scope..
        if (tok->str() == "{" && Token::Match(tok->previous(), "[;{}]")) {
            if (checkScopeForVariable(tok->next(), var, possibleInit))
                return true;
            tok = tok->link();
            continue;
        }

        // assignment with nonzero constant..
        if (Token::Match(tok->previous(), "[;{}] %var% = - %var% ;") && tok->varId() > 0)
            notzero.insert(tok->varId());

        // Inner scope..
        if (Token::simpleMatch(tok, "if (")) {
            // initialization / usage in condition..
            if (checkIfForWhileHead(tok->next(), var, suppressErrors, bool(number_of_if == 0)))
                return true;

            // checking if a not-zero variable is zero => bail out
            if (Token::Match(tok, "if ( %var% )") && notzero.find(tok->tokAt(2)->varId()) != notzero.end())
                return true;   // this scope is not fully analysed => return true

            // goto the {
            tok = tok->next()->link()->next();

            if (!tok)
                break;
            if (tok->str() == "{") {
                bool possibleInitIf(number_of_if > 0 || suppressErrors);
                const bool initif = checkScopeForVariable(tok->next(), var, &possibleInitIf);

                // goto the }
                tok = tok->link();

                if (!Token::simpleMatch(tok, "} else {")) {
                    if (initif || possibleInitIf) {
                        ++number_of_if;
                        if (number_of_if >= 2)
                            return true;
                    }
                } else {
                    // goto the {
                    tok = tok->tokAt(2);

                    bool possibleInitElse(number_of_if > 0 || suppressErrors);
                    const bool initelse = checkScopeForVariable(tok->next(), var, &possibleInitElse);

                    // goto the }
                    tok = tok->link();

                    if (initif && initelse)
                        return true;

                    if (initif || initelse || possibleInitElse)
                        ++number_of_if;
                }
            }
        }

        // = { .. }
        if (Token::simpleMatch(tok, "= {")) {
            // end token
            const Token *end = tok->next()->link();

            // If address of variable is taken in the block then bail out
            if (Token::findmatch(tok->tokAt(2), "& %varid%", end, var.varId()))
                return true;

            // Skip block
            tok = end;
            continue;
        }

        // skip sizeof / offsetof
        if (Token::Match(tok, "sizeof|typeof|offsetof|decltype ("))
            tok = tok->next()->link();

        // for..
        if (Token::simpleMatch(tok, "for (")) {
            // is variable initialized in for-head (don't report errors yet)?
            if (checkIfForWhileHead(tok->next(), var, true, false))
                return true;

            // goto the {
            const Token *tok2 = tok->next()->link()->next();

            if (tok2 && tok2->str() == "{") {
                bool possibleinit = true;
                bool init = checkScopeForVariable(tok2->next(), var, &possibleinit);

                // variable is initialized in the loop..
                if (possibleinit || init)
                    return true;

                // is variable used in for-head?
                if (!suppressErrors) {
                    checkIfForWhileHead(tok->next(), var, false, bool(number_of_if == 0));
                }
            }
        }

        // TODO: handle loops, try, etc
        if (Token::simpleMatch(tok, ") {") || Token::Match(tok, "%var% {")) {
            return true;
        }

        // bailout if there is assembler code
        if (Token::simpleMatch(tok, "asm (")) {
            return true;
        }

        if (Token::Match(tok, "return|break|continue|throw|goto"))
            ret = true;
        else if (ret && tok->str() == ";")
            return true;

        // variable is seen..
        if (tok->varId() == var.varId()) {
            // Use variable
            if (!suppressErrors && isVariableUsage(tok, var.isPointer()))
                uninitvarError(tok, tok->str());

            else
                // assume that variable is assigned
                return true;
        }
    }

    return ret;
}

bool CheckUninitVar::checkIfForWhileHead(const Token *startparanthesis, const Variable& var, bool suppressErrors, bool isuninit)
{
    const Token * const endpar = startparanthesis->link();
    for (const Token *tok = startparanthesis->next(); tok && tok != endpar; tok = tok->next()) {
        if (tok->varId() == var.varId()) {
            if (isVariableUsage(tok, var.isPointer())) {
                if (!suppressErrors)
                    uninitvarError(tok, tok->str());
                else
                    continue;
            }
            return true;
        }
        if (Token::Match(tok, "sizeof|decltype|offsetof ("))
            tok = tok->next()->link();
        if (!isuninit && tok->str() == "&&")
            suppressErrors = true;
    }
    return false;
}

bool CheckUninitVar::isVariableUsage(const Token *vartok, bool pointer) const
{
    if (vartok->previous()->str() == "return")
        return true;

    if (Token::Match(vartok->previous(), "++|--|%op%")) {
        if (vartok->previous()->str() == ">>" && _tokenizer->isCPP()) {
            // assume that variable is initialized
            return false;
        }

        // is there something like: ; "*((&var ..expr.. ="  => the variable is assigned
        if (vartok->previous()->str() == "&") {
            const Token *tok2 = vartok->tokAt(-2);
            if (Token::simpleMatch(tok2,")"))
                tok2 = tok2->link()->previous();
            while (tok2 && tok2->str() == "(")
                tok2 = tok2->previous();
            while (tok2 && tok2->str() == "*")
                tok2 = tok2->previous();
            if (Token::Match(tok2, "[;{}] *")) {
                // there is some such code before vartok: "[*]+ [(]* &"
                // determine if there is a = after vartok
                for (tok2 = vartok; tok2; tok2 = tok2->next()) {
                    if (Token::Match(tok2, "[;{}]"))
                        break;
                    if (tok2->str() == "=")
                        return false;
                }
            }
        }

        if (vartok->previous()->str() != "&" || !Token::Match(vartok->tokAt(-2), "[(,=?:]")) {
            return true;
        }
    }

    bool unknown = false;
    if (pointer && CheckNullPointer::isPointerDeRef(vartok, unknown, _tokenizer->getSymbolDatabase())) {
        // function parameter?
        bool functionParameter = false;
        if (Token::Match(vartok->tokAt(-2), "%var% (") || vartok->previous()->str() == ",")
            functionParameter = true;

        // if this is not a function parameter report this dereference as variable usage
        if (!functionParameter)
            return true;
    }

    if (_tokenizer->isCPP() && Token::Match(vartok->next(), "<<|>>")) {
        // Is variable a known POD type then this is a variable usage,
        // otherwise we assume it's not.
        const Variable *var = _tokenizer->getSymbolDatabase()->getVariableFromVarId(vartok->varId());
        if (var && var->typeStartToken()->isStandardType())
            return true;
        return false;
    }

    if (Token::Match(vartok->next(), "++|--|%op%"))
        return true;

    if (vartok->strAt(1) == "]")
        return true;

    return false;
}


void CheckUninitVar::uninitstringError(const Token *tok, const std::string &varname, bool strncpy_)
{
    reportError(tok, Severity::error, "uninitstring", "Dangerous usage of '" + varname + "'" + (strncpy_ ? " (strncpy doesn't always null-terminate it)." : " (not null-terminated)."));
}

void CheckUninitVar::uninitdataError(const Token *tok, const std::string &varname)
{
    reportError(tok, Severity::error, "uninitdata", "Memory is allocated but not initialized: " + varname);
}

void CheckUninitVar::uninitvarError(const Token *tok, const std::string &varname)
{
    reportError(tok, Severity::error, "uninitvar", "Uninitialized variable: " + varname);
}
