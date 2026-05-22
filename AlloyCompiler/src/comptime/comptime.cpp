#include "comptime.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "../util/overloaded.hpp"

namespace
{
	// -----------------------------------------------------------------------
	// Compile-time value (§6.1). Supports scalars, arrays, structs, and — for
	// B3 reflection — '#Type' values (§3.4).
	// -----------------------------------------------------------------------
	struct StructField;
	struct ComptimeTypeMember;

	// The four kinds a reflected '#Type' can have (§3.4).
	enum class ComptimeTypeKind { Struct, Enum, Primitive, Interface };

	struct ComptimeValue
	{
		enum class Tag { Error, Unit, Integer, Float, Bool, Char, String, Array, Struct, Type };

		Tag tag = Tag::Error;
		int64_t intVal = 0;        // Integer, Char codepoint, Bool (0|1)
		double floatVal = 0.0;     // Float
		uint32_t charWidth = 0;    // Char byte width
		std::string strVal;        // String
		std::vector<ComptimeValue> arrayElems;   // Array
		std::vector<StructField> structFields;   // Struct (declaration order)

		// Tag::Type — a reflected '#Type' value (§3.4).
		ComptimeTypeKind typeKind = ComptimeTypeKind::Struct;
		std::string typeName;
		std::vector<ComptimeTypeMember> typeMembers;    // struct fields / enum variants
		std::vector<std::string> typeInterfaces;        // declared interface markers

		static ComptimeValue error()             { return {}; }
		static ComptimeValue unit()              { ComptimeValue v; v.tag = Tag::Unit; return v; }
		static ComptimeValue integer(int64_t i)  { ComptimeValue v; v.tag = Tag::Integer; v.intVal = i; return v; }
		static ComptimeValue floating(double d)  { ComptimeValue v; v.tag = Tag::Float; v.floatVal = d; return v; }
		static ComptimeValue boolean(bool b)     { ComptimeValue v; v.tag = Tag::Bool; v.intVal = b ? 1 : 0; return v; }
		static ComptimeValue string(std::string s) { ComptimeValue v; v.tag = Tag::String; v.strVal = std::move(s); return v; }

		bool isError() const  { return tag == Tag::Error; }
		bool isNumber() const { return tag == Tag::Integer || tag == Tag::Float; }
		double asDouble() const { return tag == Tag::Float ? floatVal : static_cast<double>(intVal); }
	};

	// A struct field carries its source name token so a struct comptime result
	// can be substituted back as a StructInitializerExpression.
	struct StructField
	{
		std::string name;
		const Token* nameTok = nullptr;
		ComptimeValue value;
	};

	// A member of a reflected '#Type' — a struct field or an enum variant.
	struct ComptimeTypeMember
	{
		std::string name;
		ComptimeValue type;   // a Tag::Type value
	};

	// Control-flow signal threaded through statement execution.
	struct ControlFlow
	{
		enum class Kind { Normal, Break, Return };
		Kind kind = Kind::Normal;
		ComptimeValue value;
	};

	// -----------------------------------------------------------------------
	// The comptime evaluator: walks the module, interprets '#' constructs, and
	// rewrites successfully evaluated nodes (value-substitution, §6.1).
	// -----------------------------------------------------------------------
	class ComptimeEvaluator
	{
	public:
		ComptimeEvaluator(const Source& source, const ResolvedModule& resolved,
			Allocator& allocator, SynthTypeMap& synthOut)
			: m_source(source), m_resolved(resolved), m_allocator(allocator),
			  m_logger(source), m_synthOut(synthOut)
		{
		}

		void run(AST::Module& module)
		{
			AST::ListNode<AST::Definition>* node = module.definitions.ptr();
			while (node)
			{
				std::visit(Overloaded
				{
					[&](Required<AST::FunctionDefinition>& fn) { rewriteFunction(fn.value()); },
					[&](Required<AST::TypeDefinition>& td)      { evalTypeDefinition(td.value()); },
					[&](auto&) {},
				}, node->item.value().definition);
				node = node->next.ptr();
			}
		}

	private:
		// === AST walker (value-substitution rewrite) =========================

		void rewriteFunction(AST::FunctionDefinition& fn)
		{
			rewriteBlock(fn.function.value().body.value());
		}

		void rewriteBlock(AST::StatementBlock& block)
		{
			AST::ListNode<AST::Statement>* node = block.statements.ptr();
			while (node)
			{
				rewriteStmt(node->item.value());
				node = node->next.ptr();
			}
		}

		void rewriteStmt(AST::Statement& stmt)
		{
			std::visit(Overloaded
			{
				[&](Required<AST::VariableDefinitionStatement>& s) { rewriteExpr(s.value().value.value()); },
				[&](Required<AST::AssignmentStatement>& s)
				{
					rewriteExpr(s.value().target.value());
					rewriteExpr(s.value().value.value());
				},
				[&](Required<AST::ExpressionStatement>& s) { rewriteExpr(s.value().expression.value()); },
				[&](Required<AST::StatementBlock>& s)      { rewriteBlock(s.value()); },
				[&](Required<AST::IfExpression>& s)        { rewriteIf(s.value()); },
				[&](Required<AST::ForExpression>& s)       { rewriteFor(s.value()); },
				[&](Required<AST::WhileExpression>& s)     { rewriteWhile(s.value()); },
				[&](Required<AST::MatchExpression>& s)     { rewriteMatch(s.value()); },
				[&](Required<AST::BreakStatement>& s)
				{
					if (s.value().value.hasValue())
						rewriteExpr(*s.value().value.ptr());
				},
				[&](Required<AST::ReturnStatement>& s)
				{
					if (s.value().value.hasValue())
						rewriteExpr(*s.value().value.ptr());
				},
			}, stmt);
		}

		void rewriteIf(AST::IfExpression& e)
		{
			rewriteExpr(e.condition.value());
			rewriteStmt(e.thenBranch.value());
			if (e.elseBranch.hasValue())
				rewriteStmt(*e.elseBranch.ptr());
		}

		void rewriteFor(AST::ForExpression& e)
		{
			AST::ListNode<AST::Expression>* it = &e.iterables.value();
			while (it) { rewriteExpr(it->item.value()); it = it->next.ptr(); }
			rewriteStmt(e.body.value());
			if (e.elseBody.hasValue())
				rewriteStmt(*e.elseBody.ptr());
		}

		void rewriteWhile(AST::WhileExpression& e)
		{
			rewriteExpr(e.condition.value());
			rewriteStmt(e.body.value());
			if (e.elseBody.hasValue())
				rewriteStmt(*e.elseBody.ptr());
		}

		void rewriteMatch(AST::MatchExpression& e)
		{
			rewriteExpr(e.subject.value());
			AST::ListNode<AST::MatchArm>* arm = e.arms.ptr();
			while (arm)
			{
				if (arm->item.value().pattern.hasValue())
					rewriteExpr(*arm->item.value().pattern.ptr());
				rewriteStmt(arm->item.value().body.value());
				arm = arm->next.ptr();
			}
			if (e.externalElse.hasValue())
				rewriteStmt(*e.externalElse.ptr());
		}

		void rewriteExpr(AST::Expression& expr)
		{
			if (auto* ct = std::get_if<Required<AST::ComptimeExpression>>(&expr))
			{
				substituteComptime(expr, ct->value());
				return;
			}

			std::visit(Overloaded
			{
				[&](Required<AST::IdentifierExpression>&) {},
				[&](Required<AST::LiteralExpression>&) {},
				[&](Required<AST::ComptimeResultExpression>&) {},
				[&](Required<AST::IfExpression>& e)    { rewriteIf(e.value()); },
				[&](Required<AST::ForExpression>& e)   { rewriteFor(e.value()); },
				[&](Required<AST::WhileExpression>& e) { rewriteWhile(e.value()); },
				[&](Required<AST::MatchExpression>& e) { rewriteMatch(e.value()); },
				[&](Required<AST::FunctionCallExpression>& e)
				{
					rewriteExpr(e.value().function.value());
					AST::ListNode<AST::Expression>* a = e.value().arguments.ptr();
					while (a) { rewriteExpr(a->item.value()); a = a->next.ptr(); }
				},
				[&](Required<AST::MemberAccessExpression>& e) { rewriteExpr(e.value().object.value()); },
				[&](Required<AST::ArrayAccessExpression>& e)
				{
					rewriteExpr(e.value().object.value());
					rewriteExpr(e.value().index.value());
				},
				[&](Required<AST::ArrayLiteralExpression>& e)
				{
					AST::ListNode<AST::Expression>* el = e.value().elements.ptr();
					while (el) { rewriteExpr(el->item.value()); el = el->next.ptr(); }
				},
				[&](Required<AST::ArrayFillExpression>& e) { rewriteExpr(e.value().value.value()); },
				[&](Required<AST::StructInitializerExpression>& e)
				{
					AST::ListNode<AST::StructInitializerExpression::MemberInitializer>* m = e.value().initializers.ptr();
					while (m) { rewriteExpr(m->item.value().value.value()); m = m->next.ptr(); }
				},
				[&](Required<AST::LambdaExpression>& e) { rewriteBlock(e.value().function.value().body.value()); },
				[&](Required<AST::BinaryExpression>& e)
				{
					rewriteExpr(e.value().left.value());
					rewriteExpr(e.value().right.value());
				},
				[&](Required<AST::UnaryExpression>& e) { rewriteExpr(e.value().expression.value()); },
				[&](Required<AST::ComptimeExpression>&) {},   // handled above
			}, expr);
		}

		// Evaluates a '#' construct and, on success, replaces it in-place.
		void substituteComptime(AST::Expression& slot, const AST::ComptimeExpression& node)
		{
			m_failed = false;
			m_signal = {};
			m_stepBudget = STEP_BUDGET;
			m_callDepth = 0;
			m_currentHash = &node.hash;
			m_frames.clear();
			m_evaluatingConsts.clear();

			m_frames.emplace_back();
			ComptimeValue v = eval(node.inner.value());
			m_frames.clear();

			if (m_failed)
				return;   // diagnostic already reported; leave the node unevaluated

			// §3.4 — a '#Type' value exists only at compile time and is not a
			// runtime value, so the node is left unsubstituted. A `#Type` is
			// meaningful only within comptime evaluation (e.g. a `#struct_type()`
			// bound to a local inside a `#`-evaluated function); rejecting its
			// use in genuine runtime position is B3 5-6 (type synthesis).
			if (v.tag == ComptimeValue::Tag::Type)
				return;

			AST::Expression* synth = synthExpr(v, node.hash);
			if (!synth)
			{
				fail(node.hash, "Compile-time expression did not produce a substitutable value.");
				return;
			}
			slot = *synth;
		}

		// B3-5 (§3.4) — a `type T = #...` definition: evaluate the type-position
		// comptime expression and record the synthesised type for the interner.
		void evalTypeDefinition(AST::TypeDefinition& def)
		{
			auto* ct = std::get_if<Required<AST::ComptimeExpression>>(&def.baseType.value());
			if (!ct)
				return;   // an ordinary type definition — nothing to evaluate

			const AST::ComptimeExpression& node = ct->value();
			m_failed = false;
			m_signal = {};
			m_stepBudget = STEP_BUDGET;
			m_callDepth = 0;
			m_currentHash = &node.hash;
			m_frames.clear();
			m_evaluatingConsts.clear();

			m_frames.emplace_back();
			ComptimeValue v = eval(node.inner.value());
			m_frames.clear();

			if (m_failed)
				return;

			if (v.tag != ComptimeValue::Tag::Type)
			{
				fail(node.hash, "A type-position comptime expression must evaluate to a #Type.");
				return;
			}
			if (v.typeKind != ComptimeTypeKind::Struct && v.typeKind != ComptimeTypeKind::Enum)
			{
				fail(node.hash, "Only a struct or enum #Type can be synthesised as a named type.");
				return;
			}

			SynthType st;
			st.isEnum = (v.typeKind == ComptimeTypeKind::Enum);
			st.name = v.typeName;
			for (const ComptimeTypeMember& mem : v.typeMembers)
			{
				if (mem.type.tag != ComptimeValue::Tag::Type)
				{
					fail(node.hash, "A synthesised type member must itself have a #Type.");
					return;
				}
				st.members.push_back(SynthType::Member{ mem.name, mem.type.typeName });
			}
			m_synthOut[&node] = std::move(st);
		}

		// === Interpreter ====================================================

		bool stop() const { return m_failed || m_signal.kind != ControlFlow::Kind::Normal; }

		ComptimeValue eval(const AST::Expression& expr)
		{
			if (stop())
				return ComptimeValue::error();

			ComptimeValue result = ComptimeValue::error();
			std::visit(Overloaded
			{
				[&](const Required<AST::IdentifierExpression>& e) { result = evalIdentifier(e.value()); },
				[&](const Required<AST::LiteralExpression>& e)    { result = evalLiteral(e.value()); },
				[&](const Required<AST::BinaryExpression>& e)     { result = evalBinary(e.value()); },
				[&](const Required<AST::UnaryExpression>& e)      { result = evalUnary(e.value()); },
				[&](const Required<AST::IfExpression>& e)         { result = evalIf(e.value()); },
				[&](const Required<AST::WhileExpression>& e)      { result = evalWhile(e.value()); },
				[&](const Required<AST::MatchExpression>& e)      { result = evalMatch(e.value()); },
				[&](const Required<AST::FunctionCallExpression>& e) { result = evalCall(e.value()); },
				[&](const Required<AST::ComptimeExpression>& e)   { result = eval(e.value().inner.value()); },
				[&](const Required<AST::ComptimeResultExpression>& e) { result = fromResultNode(e.value()); },
				[&](const Required<AST::ForExpression>& e)               { result = evalFor(e.value()); },
				[&](const Required<AST::MemberAccessExpression>& e)      { result = evalMember(e.value()); },
				[&](const Required<AST::ArrayAccessExpression>& e)       { result = evalIndex(e.value()); },
				[&](const Required<AST::ArrayLiteralExpression>& e)      { result = evalArrayLiteral(e.value()); },
				[&](const Required<AST::ArrayFillExpression>& e)         { result = evalArrayFill(e.value()); },
				[&](const Required<AST::StructInitializerExpression>& e) { result = evalStructInit(e.value()); },
				[&](const Required<AST::LambdaExpression>&)
				{
					fail(*m_currentHash, "Compile-time evaluation of lambdas is not supported.");
				},
			}, expr);
			return result;
		}

		ComptimeValue fromResultNode(const AST::ComptimeResultExpression& node)
		{
			using RK = AST::ComptimeResultExpression::ResultKind;
			switch (node.kind)
			{
				case RK::Integer: return ComptimeValue::integer(node.intValue);
				case RK::Float:   return ComptimeValue::floating(node.floatValue);
				case RK::Bool:    return ComptimeValue::boolean(node.intValue != 0);
				case RK::Char:
				{
					ComptimeValue v = ComptimeValue::integer(node.intValue);
					v.tag = ComptimeValue::Tag::Char;
					v.charWidth = node.charWidth;
					return v;
				}
				case RK::String:
				{
					ComptimeValue v; v.tag = ComptimeValue::Tag::String; return v;
				}
				default: return ComptimeValue::error();
			}
		}

		ComptimeValue evalLiteral(const AST::LiteralExpression& lit)
		{
			std::string_view t = text(lit.value);
			switch (lit.value.kind)
			{
				case TokenKind::IntegerLiteral: return ComptimeValue::integer(parseInteger(t));
				case TokenKind::FloatLiteral:
				{
					try { return ComptimeValue::floating(std::stod(std::string(t))); }
					catch (...) { fail(lit.value, "Invalid floating-point literal."); return ComptimeValue::error(); }
				}
				case TokenKind::True:  return ComptimeValue::boolean(true);
				case TokenKind::False: return ComptimeValue::boolean(false);
				case TokenKind::StringLiteral:
				{
					ComptimeValue v;
					v.tag = ComptimeValue::Tag::String;
					if (t.size() >= 2) v.strVal = std::string(t.substr(1, t.size() - 2));
					return v;
				}
				case TokenKind::CharLiteral:
				{
					ComptimeValue v;
					v.tag = ComptimeValue::Tag::Char;
					// Best-effort byte width: span minus the two enclosing quotes.
					uint32_t w = t.size() >= 2 ? static_cast<uint32_t>(t.size() - 2) : 1;
					if (w < 1) w = 1;
					if (w > 8) w = 8;
					v.charWidth = w;
					return v;
				}
				default:
					fail(lit.value, "Unsupported literal in a compile-time expression.");
					return ComptimeValue::error();
			}
		}

		ComptimeValue evalIdentifier(const AST::IdentifierExpression& ident)
		{
			if (ident.path.value().next.hasValue())
			{
				fail(*identToken(ident), "Qualified names are not supported in compile-time evaluation.");
				return ComptimeValue::error();
			}

			const ResolvedDeclaration* decl = lookupSingle(ident);
			if (!decl)
			{
				// A built-in primitive type name reflects to a primitive #Type (§3.4).
				std::string_view nm = text(*identToken(ident));
				if (isPrimitiveTypeName(nm))
					return makeTypeValue(ComptimeTypeKind::Primitive, std::string(nm));
				fail(*identToken(ident), "Unresolved name in a compile-time expression.");
				return ComptimeValue::error();
			}

			if (auto* p = std::get_if<Required<AST::FunctionParameter>>(&decl->definition))
			{
				const AST::FunctionParameter* key = p->ptr();
				for (auto it = m_frames.rbegin(); it != m_frames.rend(); ++it)
				{
					auto found = it->find(key);
					if (found != it->end())
						return found->second;
				}
				fail(*identToken(ident), "A function parameter cannot be used in this compile-time context.");
				return ComptimeValue::error();
			}

			if (auto* var = std::get_if<Required<AST::VariableDefinitionStatement>>(&decl->definition))
			{
				const AST::VariableDefinitionStatement* key = var->ptr();
				for (auto it = m_frames.rbegin(); it != m_frames.rend(); ++it)
				{
					auto found = it->find(key);
					if (found != it->end())
						return found->second;
				}
				// Not bound in any frame — only a 'const' with a comptime-evaluable
				// initializer may be referenced this way.
				if (key->isMutable)
				{
					fail(*identToken(ident), "'{}' is not a compile-time constant.", key->name);
					return ComptimeValue::error();
				}
				if (m_evaluatingConsts.count(key))
				{
					fail(*identToken(ident), "Cyclic compile-time constant.");
					return ComptimeValue::error();
				}
				m_evaluatingConsts.insert(key);
				ComptimeValue v = eval(key->value.value());
				m_evaluatingConsts.erase(key);
				return v;
			}

			if (auto* cap = std::get_if<Required<AST::Capture>>(&decl->definition))
			{
				const AST::Capture* key = cap->ptr();
				for (auto it = m_frames.rbegin(); it != m_frames.rend(); ++it)
				{
					auto found = it->find(key);
					if (found != it->end())
						return found->second;
				}
				fail(*identToken(ident), "A capture is not available in this compile-time context.");
				return ComptimeValue::error();
			}

			// B3 (§3.4): a type name reflects to a '#Type' value.
			if (auto* td = std::get_if<Required<AST::TypeDefinition>>(&decl->definition))
			{
				std::unordered_set<const void*> visiting;
				return reflectDefinition(td->value(), visiting);
			}
			if (auto* idf = std::get_if<Required<AST::InterfaceDefinition>>(&decl->definition))
				return reflectInterface(idf->value());

			fail(*identToken(ident), "Name does not denote a compile-time value.");
			return ComptimeValue::error();
		}

		// === Composite values: arrays and structs ===========================

		ComptimeValue evalArrayLiteral(const AST::ArrayLiteralExpression& e)
		{
			ComptimeValue v;
			v.tag = ComptimeValue::Tag::Array;
			for (const AST::ListNode<AST::Expression>* el = e.elements.ptr(); el; el = el->next.ptr())
			{
				v.arrayElems.push_back(eval(el->item.value()));
				if (stop()) return ComptimeValue::error();
			}
			return v;
		}

		ComptimeValue evalArrayFill(const AST::ArrayFillExpression& e)
		{
			ComptimeValue elem = eval(e.value.value());
			if (stop()) return ComptimeValue::error();

			const int64_t n = parseInteger(text(e.size));
			if (n < 0 || n > MAX_ARRAY_SIZE)
			{
				fail(e.size, "Compile-time array-fill size is out of range.");
				return ComptimeValue::error();
			}

			ComptimeValue v;
			v.tag = ComptimeValue::Tag::Array;
			v.arrayElems.assign(static_cast<size_t>(n), elem);
			return v;
		}

		ComptimeValue evalStructInit(const AST::StructInitializerExpression& e)
		{
			ComptimeValue v;
			v.tag = ComptimeValue::Tag::Struct;
			for (const AST::ListNode<AST::StructInitializerExpression::MemberInitializer>* m = e.initializers.ptr();
				m; m = m->next.ptr())
			{
				const AST::StructInitializerExpression::MemberInitializer& mi = m->item.value();
				ComptimeValue fv = eval(mi.value.value());
				if (stop()) return ComptimeValue::error();
				v.structFields.push_back(StructField{ std::string(text(mi.name)), &mi.name, fv });
			}
			return v;
		}

		ComptimeValue evalMember(const AST::MemberAccessExpression& m)
		{
			ComptimeValue obj = eval(m.object.value());
			if (stop()) return ComptimeValue::error();
			if (obj.tag != ComptimeValue::Tag::Struct)
			{
				fail(m.memberName, "Compile-time member access requires a struct value.");
				return ComptimeValue::error();
			}
			std::string_view field = text(m.memberName);
			for (const StructField& f : obj.structFields)
				if (f.name == field)
					return f.value;
			fail(m.memberName, "Struct value has no field with this name.");
			return ComptimeValue::error();
		}

		ComptimeValue evalIndex(const AST::ArrayAccessExpression& a)
		{
			ComptimeValue arr = eval(a.object.value());
			if (stop()) return ComptimeValue::error();
			ComptimeValue idx = eval(a.index.value());
			if (stop()) return ComptimeValue::error();

			if (arr.tag != ComptimeValue::Tag::Array)
			{
				fail(*m_currentHash, "Compile-time indexing requires an array value.");
				return ComptimeValue::error();
			}
			if (idx.tag != ComptimeValue::Tag::Integer)
			{
				fail(*m_currentHash, "A compile-time array index must be an integer.");
				return ComptimeValue::error();
			}
			if (idx.intVal < 0 || idx.intVal >= static_cast<int64_t>(arr.arrayElems.size()))
			{
				fail(*m_currentHash, "Compile-time array index is out of bounds.");
				return ComptimeValue::error();
			}
			return arr.arrayElems[static_cast<size_t>(idx.intVal)];
		}

		// The built-in '.length()' method (§5.1) on an array or string value.
		ComptimeValue evalBuiltinMethod(const AST::MemberAccessExpression& m, const AST::FunctionCallExpression& call)
		{
			std::string_view method = text(m.memberName);
			ComptimeValue obj = eval(m.object.value());
			if (stop()) return ComptimeValue::error();

			// B3 (§3.4): '#Type' reflection methods.
			if (obj.tag == ComptimeValue::Tag::Type)
				return evalTypeMethod(m, call, obj);

			if (method == "length")
			{
				if (call.arguments.hasValue())
				{
					fail(m.memberName, "'.length()' takes no arguments.");
					return ComptimeValue::error();
				}
				if (obj.tag == ComptimeValue::Tag::Array)
					return ComptimeValue::integer(static_cast<int64_t>(obj.arrayElems.size()));
				if (obj.tag == ComptimeValue::Tag::String)
					return ComptimeValue::integer(static_cast<int64_t>(obj.strVal.size()));
				fail(m.memberName, "'.length()' requires an array or string value.");
				return ComptimeValue::error();
			}

			fail(m.memberName, "Compile-time evaluation of this method is not supported.");
			return ComptimeValue::error();
		}

		// Compile-time 'for' (§4.3) — iterates an array value, binding the
		// optional capture to each element. Yields a value via 'break'.
		ComptimeValue evalFor(const AST::ForExpression& e)
		{
			ComptimeValue iterable = eval(e.iterables.value().item.value());
			if (stop()) return ComptimeValue::error();
			if (iterable.tag != ComptimeValue::Tag::Array)
			{
				fail(*m_currentHash, "A compile-time 'for' loop requires an array iterable.");
				return ComptimeValue::error();
			}

			const AST::Capture* capture = nullptr;
			if (e.iterators.hasValue())
				capture = &e.iterators.value().item.value();

			for (const ComptimeValue& elem : iterable.arrayElems)
			{
				if (--m_stepBudget < 0)
				{
					fail(*m_currentHash, "Compile-time loop exceeded its evaluation step budget.");
					return ComptimeValue::error();
				}
				if (capture)
					m_frames.back()[capture] = elem;

				exec(e.body.value());
				if (m_failed) return ComptimeValue::error();
				if (m_signal.kind == ControlFlow::Kind::Break)
					return consumeBreak();
				if (m_signal.kind == ControlFlow::Kind::Return)
					return ComptimeValue::unit();
			}

			if (e.elseBody.hasValue())
			{
				exec(e.elseBody.value());
				return consumeBreak();
			}
			return ComptimeValue::unit();
		}

		// Synthesises the AST node a comptime result is substituted with: a
		// ComptimeResultExpression for scalars, an ArrayLiteralExpression or a
		// StructInitializerExpression for composites. Returns nullptr if the
		// value cannot be represented (e.g. Unit/Error).
		AST::Expression* synthExpr(const ComptimeValue& v, const Token& origin)
		{
			using RK = AST::ComptimeResultExpression::ResultKind;
			switch (v.tag)
			{
				case ComptimeValue::Tag::Integer:
				case ComptimeValue::Tag::Float:
				case ComptimeValue::Tag::Bool:
				case ComptimeValue::Tag::Char:
				case ComptimeValue::Tag::String:
				{
					RK kind =
						v.tag == ComptimeValue::Tag::Integer ? RK::Integer :
						v.tag == ComptimeValue::Tag::Float   ? RK::Float :
						v.tag == ComptimeValue::Tag::Bool    ? RK::Bool :
						v.tag == ComptimeValue::Tag::Char    ? RK::Char : RK::String;
					auto* r = m_allocator.allocate<AST::ComptimeResultExpression>(
						kind, origin, v.charWidth, v.intVal, v.floatVal);
					return m_allocator.allocate<AST::Expression>(Required<AST::ComptimeResultExpression>(r));
				}
				case ComptimeValue::Tag::Array:
				{
					if (v.arrayElems.empty())
						return nullptr;   // element type cannot be inferred
					AST::ListBuilder<AST::Expression> elems;
					for (const ComptimeValue& el : v.arrayElems)
					{
						AST::Expression* ee = synthExpr(el, origin);
						if (!ee) return nullptr;
						elems.append(Required<AST::Expression>(ee), m_allocator);
					}
					auto* arr = m_allocator.allocate<AST::ArrayLiteralExpression>(elems.head);
					return m_allocator.allocate<AST::Expression>(Required<AST::ArrayLiteralExpression>(arr));
				}
				case ComptimeValue::Tag::Struct:
				{
					using MI = AST::StructInitializerExpression::MemberInitializer;
					AST::ListBuilder<MI> members;
					for (const StructField& f : v.structFields)
					{
						if (!f.nameTok) return nullptr;
						AST::Expression* fe = synthExpr(f.value, origin);
						if (!fe) return nullptr;
						auto* mi = m_allocator.allocate<MI>(*f.nameTok, Required<AST::Expression>(fe));
						members.append(Required<MI>(mi), m_allocator);
					}
					auto* si = m_allocator.allocate<AST::StructInitializerExpression>(
						Optional<AST::NamedType>(), members.head);
					return m_allocator.allocate<AST::Expression>(Required<AST::StructInitializerExpression>(si));
				}
				default:
					return nullptr;
			}
		}

		ComptimeValue evalUnary(const AST::UnaryExpression& un)
		{
			// §6.2 pointer barrier: address-of / allocation may not occur in comptime.
			if (un.op == TokenKind::BitwiseAnd || un.op == TokenKind::New || un.op == TokenKind::Move)
			{
				fail(*m_currentHash,
					"§6.2: a compile-time expression may not take a reference or allocate a pointer.");
				return ComptimeValue::error();
			}

			ComptimeValue v = eval(un.expression.value());
			if (stop()) return ComptimeValue::error();

			switch (un.op)
			{
				case TokenKind::Not:
					if (v.tag != ComptimeValue::Tag::Bool)
					{
						fail(*m_currentHash, "Operator '!' requires a boolean operand.");
						return ComptimeValue::error();
					}
					return ComptimeValue::boolean(v.intVal == 0);
				case TokenKind::BitwiseNot:
					if (v.tag != ComptimeValue::Tag::Integer)
					{
						fail(*m_currentHash, "Operator '~' requires an integer operand.");
						return ComptimeValue::error();
					}
					return ComptimeValue::integer(~v.intVal);
				default:
					fail(*m_currentHash, "Unsupported unary operator in a compile-time expression.");
					return ComptimeValue::error();
			}
		}

		ComptimeValue evalBinary(const AST::BinaryExpression& bin)
		{
			// Short-circuiting logical operators.
			if (bin.op == TokenKind::LogicalAnd || bin.op == TokenKind::LogicalOr)
			{
				ComptimeValue l = eval(bin.left.value());
				if (stop()) return ComptimeValue::error();
				if (l.tag != ComptimeValue::Tag::Bool)
				{
					fail(*m_currentHash, "Logical operator requires boolean operands.");
					return ComptimeValue::error();
				}
				const bool lb = l.intVal != 0;
				if (bin.op == TokenKind::LogicalAnd && !lb) return ComptimeValue::boolean(false);
				if (bin.op == TokenKind::LogicalOr && lb)   return ComptimeValue::boolean(true);

				ComptimeValue r = eval(bin.right.value());
				if (stop()) return ComptimeValue::error();
				if (r.tag != ComptimeValue::Tag::Bool)
				{
					fail(*m_currentHash, "Logical operator requires boolean operands.");
					return ComptimeValue::error();
				}
				return ComptimeValue::boolean(r.intVal != 0);
			}

			ComptimeValue l = eval(bin.left.value());
			if (stop()) return ComptimeValue::error();
			ComptimeValue r = eval(bin.right.value());
			if (stop()) return ComptimeValue::error();

			return applyBinary(bin.op, l, r);
		}

		ComptimeValue applyBinary(TokenKind op, const ComptimeValue& l, const ComptimeValue& r)
		{
			// Equality / inequality — defined for matching scalar kinds.
			if (op == TokenKind::Equal || op == TokenKind::NotEqual)
			{
				bool eq = false;
				if (l.isNumber() && r.isNumber())
					eq = l.asDouble() == r.asDouble();
				else if (l.tag == ComptimeValue::Tag::Bool && r.tag == ComptimeValue::Tag::Bool)
					eq = l.intVal == r.intVal;
				else
				{
					fail(*m_currentHash, "Operands of a compile-time comparison have incompatible types.");
					return ComptimeValue::error();
				}
				return ComptimeValue::boolean(op == TokenKind::Equal ? eq : !eq);
			}

			if (!l.isNumber() || !r.isNumber())
			{
				fail(*m_currentHash, "A compile-time arithmetic operator requires numeric operands.");
				return ComptimeValue::error();
			}

			// Relational comparisons.
			switch (op)
			{
				case TokenKind::Less:         return ComptimeValue::boolean(l.asDouble() <  r.asDouble());
				case TokenKind::LessEqual:    return ComptimeValue::boolean(l.asDouble() <= r.asDouble());
				case TokenKind::Greater:      return ComptimeValue::boolean(l.asDouble() >  r.asDouble());
				case TokenKind::GreaterEqual: return ComptimeValue::boolean(l.asDouble() >= r.asDouble());
				default: break;
			}

			const bool floatOp = l.tag == ComptimeValue::Tag::Float || r.tag == ComptimeValue::Tag::Float;

			if (floatOp)
			{
				const double a = l.asDouble(), b = r.asDouble();
				switch (op)
				{
					case TokenKind::Plus:     return ComptimeValue::floating(a + b);
					case TokenKind::Minus:    return ComptimeValue::floating(a - b);
					case TokenKind::Multiply: return ComptimeValue::floating(a * b);
					case TokenKind::Divide:
						if (b == 0.0) { fail(*m_currentHash, "Compile-time division by zero."); return ComptimeValue::error(); }
						return ComptimeValue::floating(a / b);
					default:
						fail(*m_currentHash, "Unsupported operator for floating-point compile-time operands.");
						return ComptimeValue::error();
				}
			}

			const int64_t a = l.intVal, b = r.intVal;
			switch (op)
			{
				case TokenKind::Plus:       return ComptimeValue::integer(a + b);
				case TokenKind::Minus:      return ComptimeValue::integer(a - b);
				case TokenKind::Multiply:   return ComptimeValue::integer(a * b);
				case TokenKind::Divide:
					if (b == 0) { fail(*m_currentHash, "Compile-time division by zero."); return ComptimeValue::error(); }
					return ComptimeValue::integer(a / b);
				case TokenKind::Modulo:
					if (b == 0) { fail(*m_currentHash, "Compile-time division by zero."); return ComptimeValue::error(); }
					return ComptimeValue::integer(a % b);
				case TokenKind::ShiftLeft:  return ComptimeValue::integer(a << b);
				case TokenKind::ShiftRight: return ComptimeValue::integer(a >> b);
				case TokenKind::BitwiseAnd: return ComptimeValue::integer(a & b);
				case TokenKind::BitwiseOr:  return ComptimeValue::integer(a | b);
				case TokenKind::Xor:        return ComptimeValue::integer(a ^ b);
				default:
					fail(*m_currentHash, "Unsupported operator in a compile-time expression.");
					return ComptimeValue::error();
			}
		}

		ComptimeValue evalIf(const AST::IfExpression& e)
		{
			ComptimeValue cond = eval(e.condition.value());
			if (stop()) return ComptimeValue::error();
			if (cond.tag != ComptimeValue::Tag::Bool)
			{
				fail(*m_currentHash, "An 'if' condition must evaluate to a boolean at compile time.");
				return ComptimeValue::error();
			}

			if (cond.intVal != 0)
				exec(e.thenBranch.value());
			else if (e.elseBranch.hasValue())
				exec(e.elseBranch.value());

			return consumeBreak();
		}

		ComptimeValue evalWhile(const AST::WhileExpression& e)
		{
			while (true)
			{
				if (--m_stepBudget < 0)
				{
					fail(*m_currentHash, "Compile-time loop exceeded its evaluation step budget.");
					return ComptimeValue::error();
				}

				ComptimeValue cond = eval(e.condition.value());
				if (stop()) return ComptimeValue::error();
				if (cond.tag != ComptimeValue::Tag::Bool)
				{
					fail(*m_currentHash, "A 'while' condition must evaluate to a boolean at compile time.");
					return ComptimeValue::error();
				}
				if (cond.intVal == 0)
					break;

				exec(e.body.value());
				if (m_failed) return ComptimeValue::error();
				if (m_signal.kind == ControlFlow::Kind::Break)
					return consumeBreak();
				if (m_signal.kind == ControlFlow::Kind::Return)
					return ComptimeValue::unit();   // propagate
			}

			if (e.elseBody.hasValue())
			{
				exec(e.elseBody.value());
				return consumeBreak();
			}
			return ComptimeValue::unit();
		}

		ComptimeValue evalMatch(const AST::MatchExpression& e)
		{
			ComptimeValue subject = eval(e.subject.value());
			if (stop()) return ComptimeValue::error();

			const AST::MatchArm* selected = nullptr;
			for (const AST::ListNode<AST::MatchArm>* arm = e.arms.ptr(); arm; arm = arm->next.ptr())
			{
				const AST::MatchArm& a = arm->item.value();
				if (!a.pattern.hasValue())
				{
					selected = &a;   // internal catch-all 'else' arm
					break;
				}
				ComptimeValue pat = eval(a.pattern.value());
				if (stop()) return ComptimeValue::error();
				if (valuesEqual(subject, pat))
				{
					selected = &a;
					break;
				}
			}

			if (selected)
			{
				exec(selected->body.value());
				if (m_failed) return ComptimeValue::error();
				if (m_signal.kind == ControlFlow::Kind::Break)
					return consumeBreak();
				if (m_signal.kind == ControlFlow::Kind::Return)
					return ComptimeValue::unit();
			}

			if (e.externalElse.hasValue())
			{
				exec(e.externalElse.value());
				return consumeBreak();
			}
			return ComptimeValue::unit();
		}

		ComptimeValue evalCall(const AST::FunctionCallExpression& call)
		{
			// A '.method()' call — only the built-in '.length()' is supported.
			if (auto* m = std::get_if<Required<AST::MemberAccessExpression>>(&call.function.value()))
				return evalBuiltinMethod(m->value(), call);

			const auto* calleeIdent = std::get_if<Required<AST::IdentifierExpression>>(&call.function.value());
			if (!calleeIdent)
			{
				fail(*m_currentHash, "A compile-time call target must be a named function.");
				return ComptimeValue::error();
			}

			std::vector<const ResolvedDeclaration*> decls = lookupAll(calleeIdent->value());
			if (decls.empty())
			{
				// §6.4 — built-in comptime macros.
				if (!calleeIdent->value().path.value().next.hasValue())
				{
					std::string_view fname = text(*identToken(calleeIdent->value()));
					if (fname == "type_of")     return evalTypeOf(call);
					if (fname == "struct_type") return evalEmptyType(ComptimeTypeKind::Struct);
					if (fname == "enum_type")   return evalEmptyType(ComptimeTypeKind::Enum);
				}
				fail(*identToken(calleeIdent->value()), "Unresolved function in a compile-time call.");
				return ComptimeValue::error();
			}

			// Evaluate arguments left-to-right (§4.1).
			std::vector<ComptimeValue> args;
			for (const AST::ListNode<AST::Expression>* a = call.arguments.ptr(); a; a = a->next.ptr())
			{
				args.push_back(eval(a->item.value()));
				if (stop()) return ComptimeValue::error();
			}

			// Select a call target — a function or a macro (B2). 'extern' is
			// rejected (§6.2). Prefer an arity-matching candidate.
			Optional<AST::ListNode<AST::FunctionParameter>> targetParams;
			const AST::StatementBlock* targetBody = nullptr;
			bool found = false;
			for (const ResolvedDeclaration* d : decls)
			{
				if (std::holds_alternative<Required<AST::ExternDefinition>>(d->definition))
				{
					fail(*identToken(calleeIdent->value()),
						"§6.2: compile-time code may not call 'extern' functions.");
					return ComptimeValue::error();
				}
				if (auto* f = std::get_if<Required<AST::FunctionDefinition>>(&d->definition))
				{
					const AST::Function& fn = f->value().function.value();
					if (!found || listLength(fn.parameters) == args.size())
					{
						targetParams = fn.parameters;
						targetBody = &fn.body.value();
						found = true;
					}
					if (listLength(fn.parameters) == args.size()) break;
				}
				else if (auto* mc = std::get_if<Required<AST::MacroDefinition>>(&d->definition))
				{
					const AST::MacroDefinition& md = mc->value();
					if (!found || listLength(md.parameters) == args.size())
					{
						targetParams = md.parameters;
						targetBody = &md.body.value();
						found = true;
					}
					if (listLength(md.parameters) == args.size()) break;
				}
			}

			if (!found)
			{
				fail(*identToken(calleeIdent->value()),
					"Compile-time call target is not a function or macro.");
				return ComptimeValue::error();
			}

			std::vector<const AST::FunctionParameter*> params;
			targetParams.forEach([&](const Required<AST::FunctionParameter>& p)
			{
				params.push_back(p.ptr());
			});
			if (params.size() != args.size())
			{
				fail(*identToken(calleeIdent->value()), "Incorrect argument count in a compile-time call.");
				return ComptimeValue::error();
			}

			if (++m_callDepth > MAX_CALL_DEPTH)
			{
				fail(*m_currentHash, "Compile-time call recursion is too deep.");
				--m_callDepth;
				return ComptimeValue::error();
			}

			std::unordered_map<const void*, ComptimeValue> frame;
			for (size_t i = 0; i < params.size(); ++i)
				frame[params[i]] = args[i];
			m_frames.push_back(std::move(frame));

			execBlock(*targetBody);

			m_frames.pop_back();
			--m_callDepth;

			if (m_failed)
				return ComptimeValue::error();
			if (m_signal.kind == ControlFlow::Kind::Return)
			{
				ComptimeValue v = m_signal.value;
				m_signal = {};
				return v;
			}
			if (m_signal.kind == ControlFlow::Kind::Break)
			{
				m_signal = {};
				fail(*m_currentHash, "'break' outside of a loop in a compile-time function.");
				return ComptimeValue::error();
			}
			return ComptimeValue::unit();   // function returned no value
		}

		// === B3: '#Type' reflection (§3.4) ==================================

		static bool isPrimitiveTypeName(std::string_view n)
		{
			return n == "u8" || n == "u16" || n == "u32" || n == "u64"
				|| n == "i8" || n == "i16" || n == "i32" || n == "i64"
				|| n == "f32" || n == "f64" || n == "bool";
		}

		static ComptimeValue makeTypeValue(ComptimeTypeKind kind, std::string name)
		{
			ComptimeValue v;
			v.tag = ComptimeValue::Tag::Type;
			v.typeKind = kind;
			v.typeName = std::move(name);
			return v;
		}

		// The kind of a base type, determined without expanding members — used for
		// the shallow '#Type' produced at a recursive (self-referential) back-edge.
		static ComptimeTypeKind quickKind(const AST::BaseType& bt)
		{
			if (std::holds_alternative<Required<AST::StructType>>(bt)) return ComptimeTypeKind::Struct;
			if (std::holds_alternative<Required<AST::EnumType>>(bt))   return ComptimeTypeKind::Enum;
			return ComptimeTypeKind::Primitive;
		}

		ComptimeValue reflectStructType(const AST::StructType& s, std::string name,
			std::unordered_set<const void*>& visiting)
		{
			ComptimeValue v = makeTypeValue(ComptimeTypeKind::Struct, std::move(name));
			for (const AST::ListNode<AST::StructType::Member>* m = s.members.ptr(); m; m = m->next.ptr())
			{
				ComptimeTypeMember mem;
				mem.name = std::string(text(m->item.value().name));
				mem.type = reflectType(m->item.value().type.value(), visiting);
				v.typeMembers.push_back(std::move(mem));
			}
			return v;
		}

		ComptimeValue reflectEnumType(const AST::EnumType& e, std::string name,
			std::unordered_set<const void*>& visiting)
		{
			ComptimeValue v = makeTypeValue(ComptimeTypeKind::Enum, std::move(name));
			for (const AST::ListNode<AST::EnumType::Member>* m = e.members.ptr(); m; m = m->next.ptr())
			{
				ComptimeTypeMember mem;
				mem.name = std::string(text(m->item.value().name));
				if (m->item.value().payloadType.hasValue())
					mem.type = reflectType(m->item.value().payloadType.value(), visiting);
				else
					mem.type = makeTypeValue(ComptimeTypeKind::Primitive, "void");
				v.typeMembers.push_back(std::move(mem));
			}
			return v;
		}

		ComptimeValue reflectDefinition(const AST::TypeDefinition& def,
			std::unordered_set<const void*>& visiting)
		{
			const AST::BaseType& bt = def.baseType.value();

			// Self-referential back-edge — return a shallow '#Type' (kind + name).
			if (visiting.count(&def))
				return makeTypeValue(quickKind(bt), std::string(text(def.name)));
			visiting.insert(&def);

			ComptimeValue v;
			if (auto* s = std::get_if<Required<AST::StructType>>(&bt))
				v = reflectStructType(s->value(), std::string(text(def.name)), visiting);
			else if (auto* e = std::get_if<Required<AST::EnumType>>(&bt))
				v = reflectEnumType(e->value(), std::string(text(def.name)), visiting);
			else
			{
				// alias or other base type — reflect through, keep this def's name
				v = reflectBaseType(bt, visiting);
				v.typeName = std::string(text(def.name));
			}

			def.interfaces.forEach([&](const Required<const Token*>& tok)
			{
				v.typeInterfaces.push_back(std::string(text(*tok.value())));
			});

			visiting.erase(&def);
			return v;
		}

		ComptimeValue reflectInterface(const AST::InterfaceDefinition& idef)
		{
			return makeTypeValue(ComptimeTypeKind::Interface, std::string(text(idef.name)));
		}

		ComptimeValue reflectBaseType(const AST::BaseType& bt, std::unordered_set<const void*>& visiting)
		{
			return std::visit(Overloaded
			{
				[&](const Required<AST::NamedType>& n) -> ComptimeValue
				{
					const AST::IdentifierExpression& id = n.value().name.value();
					const ResolvedDeclaration* d = lookupSingle(id);
					if (d)
					{
						if (auto* td = std::get_if<Required<AST::TypeDefinition>>(&d->definition))
							return reflectDefinition(td->value(), visiting);
						if (auto* idf = std::get_if<Required<AST::InterfaceDefinition>>(&d->definition))
							return reflectInterface(idf->value());
					}
					// a built-in primitive (not in the symbol table)
					return makeTypeValue(ComptimeTypeKind::Primitive, std::string(text(*identToken(id))));
				},
				[&](const Required<AST::StructType>& s) -> ComptimeValue
				{
					return reflectStructType(s.value(), "", visiting);
				},
				[&](const Required<AST::EnumType>& e) -> ComptimeValue
				{
					return reflectEnumType(e.value(), "", visiting);
				},
				[&](const Required<AST::ArrayType>&) -> ComptimeValue
				{
					return makeTypeValue(ComptimeTypeKind::Primitive, "[array]");
				},
				[&](const Required<AST::FunctionType>&) -> ComptimeValue
				{
					return makeTypeValue(ComptimeTypeKind::Primitive, "[fn]");
				},
				[&](const Required<AST::ComptimeExpression>&) -> ComptimeValue
				{
					return makeTypeValue(ComptimeTypeKind::Primitive, "[comptime]");
				},
			}, bt);
		}

		ComptimeValue reflectType(const AST::Type& t, std::unordered_set<const void*>& visiting)
		{
			// Indirection modifiers ('*T', '&T') are ignored — reflect the base.
			if (auto* bt = std::get_if<Required<AST::BaseType>>(&t.innerType))
				return reflectBaseType(bt->value(), visiting);
			return reflectType(std::get<Required<AST::Type>>(t.innerType).value(), visiting);
		}

		// Derive a '#Type' from a comptime value's runtime shape — used by
		// '#type_of' when the argument binding has no explicit type annotation.
		ComptimeValue reflectValueShape(const ComptimeValue& val)
		{
			switch (val.tag)
			{
				case ComptimeValue::Tag::Integer: return makeTypeValue(ComptimeTypeKind::Primitive, "i32");
				case ComptimeValue::Tag::Float:   return makeTypeValue(ComptimeTypeKind::Primitive, "f32");
				case ComptimeValue::Tag::Bool:    return makeTypeValue(ComptimeTypeKind::Primitive, "bool");
				case ComptimeValue::Tag::Char:    return makeTypeValue(ComptimeTypeKind::Primitive, "u32");
				case ComptimeValue::Tag::String:  return makeTypeValue(ComptimeTypeKind::Primitive, "&[u8]");
				case ComptimeValue::Tag::Array:   return makeTypeValue(ComptimeTypeKind::Primitive, "[array]");
				case ComptimeValue::Tag::Struct:
				{
					ComptimeValue v = makeTypeValue(ComptimeTypeKind::Struct, "");
					for (const StructField& f : val.structFields)
					{
						ComptimeTypeMember mem;
						mem.name = f.name;
						mem.type = reflectValueShape(f.value);
						v.typeMembers.push_back(std::move(mem));
					}
					return v;
				}
				default: return ComptimeValue::error();
			}
		}

		static bool typeEquals(const ComptimeValue& a, const ComptimeValue& b)
		{
			if (a.tag != ComptimeValue::Tag::Type || b.tag != ComptimeValue::Tag::Type) return false;
			if (a.typeKind != b.typeKind) return false;
			if (a.typeName != b.typeName) return false;
			if (a.typeMembers.size() != b.typeMembers.size()) return false;
			for (size_t i = 0; i < a.typeMembers.size(); ++i)
			{
				if (a.typeMembers[i].name != b.typeMembers[i].name) return false;
				if (!typeEquals(a.typeMembers[i].type, b.typeMembers[i].type)) return false;
			}
			return true;
		}

		// Returns a pointer to the stored ComptimeValue of a frame-bound binding,
		// or nullptr if the expression is not such an lvalue.
		ComptimeValue* lvalueOf(const AST::Expression& e)
		{
			const auto* id = std::get_if<Required<AST::IdentifierExpression>>(&e);
			if (!id) return nullptr;
			const ResolvedDeclaration* decl = lookupSingle(id->value());
			if (!decl) return nullptr;
			const void* key = nullptr;
			if (auto* v = std::get_if<Required<AST::VariableDefinitionStatement>>(&decl->definition))
				key = v->ptr();
			else if (auto* p = std::get_if<Required<AST::FunctionParameter>>(&decl->definition))
				key = p->ptr();
			else if (auto* c = std::get_if<Required<AST::Capture>>(&decl->definition))
				key = c->ptr();
			if (!key) return nullptr;
			for (auto it = m_frames.rbegin(); it != m_frames.rend(); ++it)
			{
				auto found = it->find(key);
				if (found != it->end())
					return &found->second;
			}
			return nullptr;
		}

		// §6.4 — '#type_of(expr)': the '#Type' of the argument's type.
		ComptimeValue evalTypeOf(const AST::FunctionCallExpression& call)
		{
			if (!call.arguments.hasValue() || call.arguments.value().next.hasValue())
			{
				fail(*m_currentHash, "#type_of expects exactly one argument.");
				return ComptimeValue::error();
			}
			const AST::Expression& arg = call.arguments.value().item.value();

			// A binding with an explicit type annotation — reflect that annotation.
			if (const auto* id = std::get_if<Required<AST::IdentifierExpression>>(&arg))
			{
				const ResolvedDeclaration* d = lookupSingle(id->value());
				if (d)
				{
					std::unordered_set<const void*> visiting;
					if (auto* p = std::get_if<Required<AST::FunctionParameter>>(&d->definition))
						return reflectType(p->value().type.value(), visiting);
					if (auto* vd = std::get_if<Required<AST::VariableDefinitionStatement>>(&d->definition))
						if (vd->value().type.hasValue())
							return reflectType(vd->value().type.value(), visiting);
				}
			}

			// Otherwise evaluate the argument and reflect its value shape.
			ComptimeValue v = eval(arg);
			if (stop()) return ComptimeValue::error();
			ComptimeValue t = reflectValueShape(v);
			if (t.isError())
				fail(*m_currentHash, "#type_of could not determine the type of its argument.");
			return t;
		}

		// §6.4 — '#struct_type()' / '#enum_type()': a fresh, empty '#Type'.
		ComptimeValue evalEmptyType(ComptimeTypeKind kind)
		{
			return makeTypeValue(kind, "");
		}

		// The '#Type' reflection methods (§3.4), dispatched from evalBuiltinMethod.
		ComptimeValue evalTypeMethod(const AST::MemberAccessExpression& m,
			const AST::FunctionCallExpression& call, const ComptimeValue& obj)
		{
			std::string_view method = text(m.memberName);

			std::vector<const AST::Expression*> args;
			for (const AST::ListNode<AST::Expression>* a = call.arguments.ptr(); a; a = a->next.ptr())
				args.push_back(&a->item.value());

			if (method == "is_struct")    return ComptimeValue::boolean(obj.typeKind == ComptimeTypeKind::Struct);
			if (method == "is_enum")      return ComptimeValue::boolean(obj.typeKind == ComptimeTypeKind::Enum);
			if (method == "is_primitive") return ComptimeValue::boolean(obj.typeKind == ComptimeTypeKind::Primitive);
			if (method == "is_interface") return ComptimeValue::boolean(obj.typeKind == ComptimeTypeKind::Interface);
			if (method == "name")         return ComptimeValue::string(obj.typeName);

			if (method == "member_names")
			{
				ComptimeValue arr;
				arr.tag = ComptimeValue::Tag::Array;
				for (const ComptimeTypeMember& mem : obj.typeMembers)
					arr.arrayElems.push_back(ComptimeValue::string(mem.name));
				return arr;
			}
			if (method == "member_types")
			{
				ComptimeValue arr;
				arr.tag = ComptimeValue::Tag::Array;
				for (const ComptimeTypeMember& mem : obj.typeMembers)
					arr.arrayElems.push_back(mem.type);
				return arr;
			}

			if (method == "implements_interface")
			{
				if (args.size() != 1)
				{
					fail(m.memberName, "implements_interface expects one argument.");
					return ComptimeValue::error();
				}
				ComptimeValue other = eval(*args[0]);
				if (stop()) return ComptimeValue::error();
				if (other.tag != ComptimeValue::Tag::Type)
				{
					fail(m.memberName, "implements_interface expects a #Type argument.");
					return ComptimeValue::error();
				}
				for (const std::string& i : obj.typeInterfaces)
					if (i == other.typeName)
						return ComptimeValue::boolean(true);
				return ComptimeValue::boolean(false);
			}

			if (method == "equals")
			{
				if (args.size() != 1)
				{
					fail(m.memberName, "equals expects one argument.");
					return ComptimeValue::error();
				}
				ComptimeValue other = eval(*args[0]);
				if (stop()) return ComptimeValue::error();
				if (other.tag != ComptimeValue::Tag::Type)
				{
					fail(m.memberName, "equals expects a #Type argument.");
					return ComptimeValue::error();
				}
				return ComptimeValue::boolean(typeEquals(obj, other));
			}

			if (method == "add_member")
			{
				if (args.size() != 2)
				{
					fail(m.memberName, "add_member expects two arguments.");
					return ComptimeValue::error();
				}
				ComptimeValue* lv = lvalueOf(m.object.value());
				if (!lv || lv->tag != ComptimeValue::Tag::Type)
				{
					fail(m.memberName, "add_member requires a mutable #Type binding.");
					return ComptimeValue::error();
				}
				ComptimeValue nameV = eval(*args[0]);
				if (stop()) return ComptimeValue::error();
				ComptimeValue typeV = eval(*args[1]);
				if (stop()) return ComptimeValue::error();
				if (nameV.tag != ComptimeValue::Tag::String)
				{
					fail(m.memberName, "add_member: the member name must be a string.");
					return ComptimeValue::error();
				}
				if (typeV.tag != ComptimeValue::Tag::Type)
				{
					fail(m.memberName, "add_member: the member type must be a #Type.");
					return ComptimeValue::error();
				}
				ComptimeTypeMember mem;
				mem.name = nameV.strVal;
				mem.type = typeV;
				lv->typeMembers.push_back(std::move(mem));
				return ComptimeValue::unit();
			}

			if (method == "remove_member")
			{
				if (args.size() != 1)
				{
					fail(m.memberName, "remove_member expects one argument.");
					return ComptimeValue::error();
				}
				ComptimeValue* lv = lvalueOf(m.object.value());
				if (!lv || lv->tag != ComptimeValue::Tag::Type)
				{
					fail(m.memberName, "remove_member requires a mutable #Type binding.");
					return ComptimeValue::error();
				}
				ComptimeValue nameV = eval(*args[0]);
				if (stop()) return ComptimeValue::error();
				if (nameV.tag != ComptimeValue::Tag::String)
				{
					fail(m.memberName, "remove_member: the member name must be a string.");
					return ComptimeValue::error();
				}
				for (auto it = lv->typeMembers.begin(); it != lv->typeMembers.end(); ++it)
					if (it->name == nameV.strVal)
					{
						lv->typeMembers.erase(it);
						break;
					}
				return ComptimeValue::unit();
			}

			fail(m.memberName, "Unknown #Type method.");
			return ComptimeValue::error();
		}

		// === Statement execution ============================================

		void execBlock(const AST::StatementBlock& block)
		{
			for (const AST::ListNode<AST::Statement>* s = block.statements.ptr(); s; s = s->next.ptr())
			{
				exec(s->item.value());
				if (stop())
					return;
			}
		}

		void exec(const AST::Statement& stmt)
		{
			if (stop())
				return;

			if (--m_stepBudget < 0)
			{
				fail(*m_currentHash, "Compile-time evaluation exceeded its step budget.");
				return;
			}

			std::visit(Overloaded
			{
				[&](const Required<AST::VariableDefinitionStatement>& s)
				{
					ComptimeValue v = eval(s.value().value.value());
					if (stop()) return;
					m_frames.back()[&s.value()] = v;
				},
				[&](const Required<AST::AssignmentStatement>& s)      { execAssignment(s.value()); },
				[&](const Required<AST::ExpressionStatement>& s)      { eval(s.value().expression.value()); },
				[&](const Required<AST::StatementBlock>& s)           { execBlock(s.value()); },
				[&](const Required<AST::IfExpression>& s)             { evalIf(s.value()); },
				[&](const Required<AST::WhileExpression>& s)          { evalWhile(s.value()); },
				[&](const Required<AST::MatchExpression>& s)          { evalMatch(s.value()); },
				[&](const Required<AST::ForExpression>& s)            { evalFor(s.value()); },
				[&](const Required<AST::BreakStatement>& s)
				{
					ComptimeValue v = ComptimeValue::unit();
					if (s.value().value.hasValue())
					{
						v = eval(s.value().value.value());
						if (stop()) return;
					}
					m_signal = ControlFlow{ ControlFlow::Kind::Break, v };
				},
				[&](const Required<AST::ReturnStatement>& s)
				{
					ComptimeValue v = ComptimeValue::unit();
					if (s.value().value.hasValue())
					{
						v = eval(s.value().value.value());
						if (stop()) return;
					}
					m_signal = ControlFlow{ ControlFlow::Kind::Return, v };
				},
			}, stmt);
		}

		void execAssignment(const AST::AssignmentStatement& assign)
		{
			const auto* targetIdent = std::get_if<Required<AST::IdentifierExpression>>(&assign.target.value());
			if (!targetIdent)
			{
				fail(*m_currentHash, "A compile-time assignment target must be a simple variable.");
				return;
			}

			const ResolvedDeclaration* decl = lookupSingle(targetIdent->value());
			const void* key = nullptr;
			if (decl)
			{
				if (auto* var = std::get_if<Required<AST::VariableDefinitionStatement>>(&decl->definition))
					key = var->ptr();
				else if (auto* p = std::get_if<Required<AST::FunctionParameter>>(&decl->definition))
					key = p->ptr();
			}
			if (!key)
			{
				fail(*identToken(targetIdent->value()), "Unresolved assignment target in compile-time code.");
				return;
			}

			std::unordered_map<const void*, ComptimeValue>* frame = nullptr;
			for (auto it = m_frames.rbegin(); it != m_frames.rend(); ++it)
			{
				if (it->count(key)) { frame = &*it; break; }
			}
			if (!frame)
			{
				fail(*identToken(targetIdent->value()), "Assignment to a non-comptime variable.");
				return;
			}

			ComptimeValue rhs = eval(assign.value.value());
			if (stop()) return;

			if (assign.op == TokenKind::Assign)
			{
				(*frame)[key] = rhs;
				return;
			}

			// Compound assignment: apply the matching arithmetic operator.
			TokenKind arith;
			switch (assign.op)
			{
				case TokenKind::PlusAssign:       arith = TokenKind::Plus;       break;
				case TokenKind::MinusAssign:      arith = TokenKind::Minus;      break;
				case TokenKind::MultiplyAssign:   arith = TokenKind::Multiply;   break;
				case TokenKind::DivideAssign:     arith = TokenKind::Divide;     break;
				case TokenKind::ModuloAssign:     arith = TokenKind::Modulo;     break;
				case TokenKind::ShiftLeftAssign:  arith = TokenKind::ShiftLeft;  break;
				case TokenKind::ShiftRightAssign: arith = TokenKind::ShiftRight; break;
				case TokenKind::AndAssign:        arith = TokenKind::BitwiseAnd; break;
				case TokenKind::OrAssign:         arith = TokenKind::BitwiseOr;  break;
				case TokenKind::XorAssign:        arith = TokenKind::Xor;        break;
				default:
					fail(*m_currentHash, "Unsupported compile-time assignment operator.");
					return;
			}
			ComptimeValue updated = applyBinary(arith, (*frame)[key], rhs);
			if (stop()) return;
			(*frame)[key] = updated;
		}

		// === Helpers ========================================================

		// Consumes a pending Break signal as the value of the enclosing
		// loop/match/if. A Return signal is left to propagate.
		ComptimeValue consumeBreak()
		{
			if (m_signal.kind == ControlFlow::Kind::Break)
			{
				ComptimeValue v = m_signal.value;
				m_signal = {};
				return v;
			}
			return ComptimeValue::unit();
		}

		static bool valuesEqual(const ComptimeValue& a, const ComptimeValue& b)
		{
			if (a.isNumber() && b.isNumber())
				return a.asDouble() == b.asDouble();
			if (a.tag == ComptimeValue::Tag::Bool && b.tag == ComptimeValue::Tag::Bool)
				return a.intVal == b.intVal;
			return false;
		}

		const ResolvedDeclaration* lookupSingle(const AST::IdentifierExpression& id) const
		{
			auto it = m_resolved.names.find(&id);
			if (it == m_resolved.names.end() || it->second.empty())
				return nullptr;
			return it->second.front();
		}

		std::vector<const ResolvedDeclaration*> lookupAll(const AST::IdentifierExpression& id) const
		{
			auto it = m_resolved.names.find(&id);
			if (it == m_resolved.names.end())
				return {};
			return it->second;
		}

		static size_t listLength(const Optional<AST::ListNode<AST::FunctionParameter>>& list)
		{
			size_t n = 0;
			list.forEach([&](const Required<AST::FunctionParameter>&) { ++n; });
			return n;
		}

		static const Token* identToken(const AST::IdentifierExpression& id)
		{
			return id.path.value().item.value();
		}

		int64_t parseInteger(std::string_view t) const
		{
			int base = 10;
			size_t i = 0;
			if (t.size() > 2 && t[0] == '0')
			{
				if (t[1] == 'x' || t[1] == 'X') { base = 16; i = 2; }
				else if (t[1] == 'b' || t[1] == 'B') { base = 2; i = 2; }
				else if (t[1] == 'o' || t[1] == 'O') { base = 8; i = 2; }
			}
			int64_t v = 0;
			for (; i < t.size(); ++i)
			{
				const char c = t[i];
				int d;
				if (c >= '0' && c <= '9')      d = c - '0';
				else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
				else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
				else continue;
				v = v * base + d;
			}
			return v;
		}

		std::string_view text(const Token& t) const
		{
			return std::string_view(m_source.data).substr(
				t.start.index, t.end.index - t.start.index);
		}

		template <typename... Args>
		void fail(const Token& at, const std::string& format, Args&&... args)
		{
			if (m_failed)
				return;   // report only the first failure per comptime construct
			m_failed = true;
			m_logger.logErrorInRange(at, at, format, std::forward<Args>(args)...);
		}

		static constexpr int STEP_BUDGET = 1'000'000;
		static constexpr int MAX_CALL_DEPTH = 256;
		static constexpr int64_t MAX_ARRAY_SIZE = 1 << 20;

		const Source& m_source;
		const ResolvedModule& m_resolved;
		Allocator& m_allocator;
		Logger m_logger;

		bool m_failed = false;
		ControlFlow m_signal;
		int m_stepBudget = STEP_BUDGET;
		int m_callDepth = 0;
		const Token* m_currentHash = nullptr;

		std::vector<std::unordered_map<const void*, ComptimeValue>> m_frames;
		std::unordered_set<const void*> m_evaluatingConsts;
		SynthTypeMap& m_synthOut;
	};
}

Status comptimeEval(
	const Source& source,
	AST::Module& module,
	const ResolvedModule& resolved,
	Allocator& allocator,
	SynthTypeMap& synthOut)
{
	ComptimeEvaluator evaluator(source, resolved, allocator, synthOut);
	evaluator.run(module);
	return Status::Ok;
}
