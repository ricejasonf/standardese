// Copyright (C) 2017 Jonathan Müller <jonathanmueller.dev@gmail.com>
// This file is subject to the license terms in the LICENSE file
// found in the top-level directory of this distribution.

#include <standardese/doc_entity.hpp>

#include <cassert>
#include <stack>

#include <cppast/cpp_enum.hpp>
#include <cppast/cpp_entity_kind.hpp>
#include <cppast/cpp_friend.hpp>
#include <cppast/cpp_language_linkage.hpp>
#include <cppast/cpp_member_function.hpp>
#include <cppast/cpp_member_variable.hpp>
#include <cppast/cpp_namespace.hpp>
#include <cppast/cpp_preprocessor.hpp>
#include <cppast/cpp_static_assert.hpp>
#include <cppast/visitor.hpp>

#include <standardese/markup/entity_kind.hpp>
#include <standardese/markup/heading.hpp>
#include <standardese/markup/link.hpp>
#include <standardese/comment.hpp>

#include "entity_visitor.hpp"
#include "get_special_entity.hpp"

using namespace standardese;

const char* synopsis_config::default_hidden_name() noexcept
{
    return "'hidden'";
}

unsigned synopsis_config::default_tab_width() noexcept
{
    return 4u;
}

synopsis_config::flags synopsis_config::default_flags() noexcept
{
    return synopsis_config::show_group_output_section;
}

generation_config::flags generation_config::default_flags() noexcept
{
    return generation_config::inline_doc;
}

namespace
{
    // tag object to mark an excluded entity
    doc_excluded_entity excluded_entity;
}

bool doc_entity::is_excluded() const noexcept
{
    auto result = this == &excluded_entity;
    assert(!result || kind() == excluded);
    return result;
}

//=== synopsis generation ===//
namespace
{
    const cppast::cpp_entity& get_real_entity(const cppast::cpp_entity& entity)
    {
        if (cppast::is_templated(entity) || cppast::is_friended(entity))
            return entity.parent().value();
        else
            return entity;
    }

    const doc_entity* get_doc_entity(const cppast::cpp_entity& entity)
    {
        return static_cast<const doc_entity*>(entity.user_data());
    }

    bool is_in_group(const doc_entity* e)
    {
        return e && e->kind() == doc_entity::cpp_entity
               && static_cast<const doc_cpp_entity*>(e)->in_member_group();
    }

    std::string get_entity_name(const cppast::cpp_entity& entity)
    {
        if (entity.kind() == cppast::cpp_friend::kind())
        {
            auto& friend_ = static_cast<const cppast::cpp_friend&>(entity);
            if (friend_.entity())
                return friend_.entity().value().name();
        }

        return entity.name();
    }

    cppast::code_generator::generation_options get_exclude_mode(
        type_safe::optional_ref<const comment::metadata> metadata)
    {
        if (!metadata || !metadata.value().exclude())
            return {};

        switch (metadata.value().exclude().value())
        {
        case comment::exclude_mode::entity:
            assert(false);
        case comment::exclude_mode::return_type:
            return cppast::code_generator::exclude_return;
        case comment::exclude_mode::target:
            return cppast::code_generator::exclude_target;
        }

        assert(false);
        return {};
    }

    bool generate_output_section(const cppast::code_generator::output& code, bool is_main,
                                 const comment::metadata& metadata)
    {
        if (!is_main && metadata.output_section())
        {
            code << cppast::comment("//=== ") << cppast::comment(metadata.output_section().value())
                 << cppast::comment(" ===//") << cppast::newl;
            return true;
        }
        else
            return false;
    }

    void generate_output_section(const cppast::code_generator::output& code, bool is_main,
                                 bool show_group_section, const comment::metadata& metadata,
                                 const std::string&, type_safe::optional<unsigned> group_member_no)
    {
        if (generate_output_section(code, is_main, metadata))
            return;
        else if (!is_main && metadata.group() && show_group_section
                 && metadata.group().value().output_section() && group_member_no == 1u)
            code << cppast::comment("//=== ")
                 << cppast::comment(metadata.group().value().output_section().value())
                 << cppast::comment(" ===//") << cppast::newl;
    }

    void generate_group_number(const cppast::code_generator::output& code,
                               const comment::metadata&              metadata,
                               type_safe::optional<unsigned>         group_member_no)
    {
        assert(metadata.group().has_value() == group_member_no.has_value());

        if (metadata.group())
        {
            if (group_member_no.value() != 1u)
                code << cppast::newl;
            code << cppast::comment("(" + std::to_string(group_member_no.value()) + ") ");
        }
    }

    void generate_synopsis_override(const cppast::code_generator::output& code,
                                    const comment::metadata&              metadata)
    {
        if (metadata.synopsis())
            code << cppast::token_seq(metadata.synopsis().value());
    }
}

class standardese::detail::markdown_code_generator : public cppast::code_generator
{
public:
    markdown_code_generator(type_safe::object_ref<const synopsis_config>          config,
                            type_safe::object_ref<const cppast::cpp_entity_index> index)
    : config_(config),
      index_(index),
      builder_(markup::block_id(), "cpp"),
      level_(0u),
      need_indent_(false),
      allow_group_(false)
    {
    }

    std::unique_ptr<markup::code_block> finish()
    {
        return builder_.finish();
    }

private:
    bool is_main_entity(const cppast::cpp_entity& e) const noexcept
    {
        auto doc_e = get_doc_entity(e);
        if (is_in_group(doc_e))
            // it is main if the first member of group is main
            return &get_real_entity(
                       static_cast<const doc_cpp_entity&>(*doc_e->parent().value().begin())
                           .entity())
                   == &main_entity();
        else
            return &get_real_entity(e) == &main_entity();
    }

    cppast::formatting do_get_formatting() const override
    {
        return cppast::formatting_flags::brace_nl | cppast::formatting_flags::comma_ws
               | cppast::formatting_flags::operator_ws;
    }

    cppast::code_generator::generation_options do_get_options(
        const cppast::cpp_entity& e_, cppast::cpp_access_specifier_kind) override
    {
        auto& e = get_real_entity(e_);

        cppast::code_generator::generation_options result;
        if (!config_->is_flag_set(synopsis_config::show_complex_noexcept))
            result |= generation_flags::exclude_noexcept_condition;
        else if (e.kind() == cppast::cpp_macro_definition::kind()
                 && !config_->is_flag_set(synopsis_config::show_macro_replacement))
            result |= generation_flags::declaration;

        auto entity = get_doc_entity(e);
        if (allow_group_ == false && entity && entity->kind() == doc_entity::cpp_entity
            && static_cast<const doc_cpp_entity*>(entity)->is_group_main() == false)
            // non main group entity not allowed
            result |= generation_flags::exclude;
        else if (entity && !entity->is_excluded())
            result |=
                type_safe::combo(entity->do_get_generation_options(*config_, is_main_entity(e)));
        else if (entity && entity->is_excluded())
            result |= generation_flags::exclude;
        else
            result |= generation_flags::declaration;

        return result;
    }

    void on_begin(const output& out, const cppast::cpp_entity& e) override
    {
        // on_end() will only be called if the entity is actually printed
        // so check whether or not it will be printed before pushing
        // also don't push templated/friended entities
        if (out && !cppast::is_templated(e) && !cppast::is_friended(e))
            entities_.push(type_safe::ref(e));

        if (auto entity = get_doc_entity(e))
            entity->do_generate_synopsis_prefix(out, *config_, is_main_entity(e));
    }

    void on_end(const output&, const cppast::cpp_entity& e) override
    {
        if (!cppast::is_templated(e) && !cppast::is_friended(e))
        {
            assert(entities_.top() == e);
            entities_.pop();

            auto doc_e = get_doc_entity(e);
            if (doc_e && doc_e->kind() == doc_entity::member_group)
            {
                // render remaining entities of group
                allow_group_.set();

                auto first = true;
                for (auto& child : static_cast<const doc_member_group_entity&>(*doc_e))
                {
                    assert(child.kind() == doc_entity::cpp_entity);
                    auto& child_e = static_cast<const doc_cpp_entity&>(child);
                    if (first)
                        first = false;
                    else
                        generate_code(child_e.entity());
                }

                allow_group_.reset();
            }
        }
    }

    void do_indent() override
    {
        level_ += config_->tab_width();
    }

    void do_unindent() override
    {
        if (level_ >= config_->tab_width())
            level_ -= config_->tab_width();
    }

    void do_write_token_seq(cppast::string_view tokens) override
    {
        update_indent();
        builder_.add_child(markup::text::build(tokens.c_str()));
    }

    void do_write_keyword(cppast::string_view keyword) override
    {
        update_indent();
        builder_.add_child(markup::code_block::keyword::build(keyword.c_str()));
    }

    void write_identifier(cppast::string_view identifier)
    {
        if (identifier.length() > 0u)
            builder_.add_child(markup::code_block::identifier::build(identifier.c_str()));
    }

    bool is_documented(const doc_entity& entity) const
    {
        if (entity.parent() && entity.parent().value().kind() == doc_entity::member_group)
            return is_documented(entity.parent().value());
        else
            return entity.comment()
                   && (entity.comment().value().brief_section()
                       || !entity.comment().value().sections().empty());
    }

    void write_link(const doc_entity& entity, cppast::string_view name)
    {
        if (is_documented(entity))
        {
            // only generate link if the entity has actual documentation
            markup::documentation_link::builder link(entity.link_name());
            link.add_child(markup::code_block::identifier::build(name.c_str()));
            builder_.add_child(link.finish());
        }
        else if (entity.is_excluded())
            write_excluded();
        else
            write_identifier(name);
    }

    void do_write_identifier(cppast::string_view identifier) override
    {
        update_indent();

        auto cur_e      = entities_.top();
        auto doc_e      = get_doc_entity(*cur_e);
        auto needs_link = !is_main_entity(*cur_e) && identifier.c_str() == get_entity_name(*cur_e);

        if (needs_link && doc_e)
            write_link(*doc_e, identifier);
        else
            write_identifier(identifier);
    }

    void do_write_reference(type_safe::array_ref<const cppast::cpp_entity_id> id,
                            cppast::string_view                               name) override
    {
        update_indent();

        auto entity = index_->lookup(id[0u]); // pick first if overloaded
        if (!entity)
        {
            auto ns = index_->lookup_namespace(id[0u]);
            if (ns.size() > 0u)
                entity = ns[0u];
        }

        if (entity && get_doc_entity(entity.value()))
            write_link(*get_doc_entity(entity.value()), name);
        else
            write_identifier(name);
    }

    void do_write_punctuation(cppast::string_view punct) override
    {
        update_indent();
        builder_.add_child(markup::code_block::punctuation::build(punct.c_str()));
    }

    void do_write_str_literal(cppast::string_view str) override
    {
        update_indent();
        builder_.add_child(markup::code_block::string_literal::build(str.c_str()));
    }

    void do_write_int_literal(cppast::string_view str) override
    {
        update_indent();
        builder_.add_child(markup::code_block::int_literal::build(str.c_str()));
    }

    void do_write_float_literal(cppast::string_view str) override
    {
        update_indent();
        builder_.add_child(markup::code_block::float_literal::build(str.c_str()));
    }

    void do_write_preprocessor(cppast::string_view punct) override
    {
        update_indent();
        builder_.add_child(markup::code_block::preprocessor::build(punct.c_str()));
    }

    void write_excluded()
    {
        update_indent();
        builder_.add_child(markup::code_block::identifier::build(config_->hidden_name()));
    }

    void do_write_excluded(const cppast::cpp_entity&) override
    {
        write_excluded();
    }

    void do_write_newline() override
    {
        builder_.add_child(markup::soft_break::build());
        need_indent_.set();
    }

    void do_write_whitespace() override
    {
        update_indent();
        builder_.add_child(markup::text::build(" "));
    }

    void update_indent()
    {
        if (need_indent_.try_reset())
            builder_.add_child(markup::text::build(std::string(level_, ' ')));
    }

    type_safe::object_ref<const synopsis_config>          config_;
    type_safe::object_ref<const cppast::cpp_entity_index> index_;

    markup::code_block::builder builder_;

    std::stack<type_safe::object_ref<const cppast::cpp_entity>> entities_;

    unsigned        level_;
    type_safe::flag need_indent_;
    type_safe::flag allow_group_;
};

std::unique_ptr<markup::code_block> standardese::generate_synopsis(
    const synopsis_config& config, const cppast::cpp_entity_index& index, const doc_entity& entity)
{
    if (entity.kind() == doc_entity::cpp_entity
        && static_cast<const doc_cpp_entity&>(entity).in_member_group())
        return generate_synopsis(config, index, entity.parent().value());
    else
    {
        detail::markdown_code_generator generator(type_safe::ref(config), type_safe::ref(index));
        entity.do_generate_code(generator);
        return generator.finish();
    }
}

cppast::code_generator::generation_options doc_cpp_entity::do_get_generation_options(
    const synopsis_config&, bool is_main) const
{
    auto metadata =
        comment().map([](const comment::doc_comment& c) { return type_safe::ref(c.metadata()); });

    auto options = get_exclude_mode(metadata);
    if (!is_main)
        options |= cppast::code_generator::declaration;
    if (metadata && metadata.value().synopsis())
        options |= cppast::code_generator::custom;

    return options;
}

void doc_cpp_entity::do_generate_synopsis_prefix(const cppast::code_generator::output& code,
                                                 const synopsis_config& config, bool is_main) const
{
    auto metadata =
        comment().map([](const comment::doc_comment& c) { return type_safe::ref(c.metadata()); });

    if (metadata)
    {
        generate_output_section(code, is_main,
                                config.is_flag_set(synopsis_config::show_group_output_section),
                                metadata.value(), link_name(), group_member_no_);
        if (is_main)
            generate_group_number(code, metadata.value(), group_member_no_);

        generate_synopsis_override(code, metadata.value());
    }
}

void doc_cpp_entity::do_generate_code(cppast::code_generator& generator) const
{
    cppast::generate_code(generator, entity());
}

cppast::code_generator::generation_options doc_member_group_entity::do_get_generation_options(
    const synopsis_config& config, bool is_main) const
{
    return begin()->do_get_generation_options(config, is_main);
}

void doc_member_group_entity::do_generate_synopsis_prefix(
    const cppast::code_generator::output& output, const synopsis_config& config, bool is_main) const
{
    begin()->do_generate_synopsis_prefix(output, config, is_main);
}

void doc_member_group_entity::do_generate_code(cppast::code_generator& generator) const
{
    begin()->do_generate_code(generator);
}

cppast::code_generator::generation_options doc_cpp_namespace::do_get_generation_options(
    const synopsis_config&, bool) const
{
    // always generate everything of a namespace
    return {};
}

void doc_cpp_namespace::do_generate_synopsis_prefix(const cppast::code_generator::output& code,
                                                    const synopsis_config&, bool is_main) const
{
    auto metadata =
        comment().map([](const comment::doc_comment& c) { return type_safe::ref(c.metadata()); });

    if (metadata)
        // only support output section
        generate_output_section(code, is_main, metadata.value());
}

void doc_cpp_namespace::do_generate_code(cppast::code_generator& generator) const
{
    cppast::generate_code(generator, namespace_());
}

cppast::code_generator::generation_options doc_cpp_file::do_get_generation_options(
    const synopsis_config&, bool is_main) const
{
    assert(is_main);
    return {};
}

void doc_cpp_file::do_generate_code(cppast::code_generator& generator) const
{
    cppast::generate_code(generator, file());
}

//=== documentation builder ===//
namespace
{
    const char* get_entity_kind_spelling(const cppast::cpp_entity& e)
    {
        switch (e.kind())
        {
        case cppast::cpp_entity_kind::file_t:
            return "Header file";

        case cppast::cpp_entity_kind::macro_definition_t:
            return "Macro";
        case cppast::cpp_entity_kind::include_directive_t:
            return "Inclusion directive";

        case cppast::cpp_entity_kind::language_linkage_t:
            return "Language linkage";
        case cppast::cpp_entity_kind::namespace_t:
            return "Namespace";
        case cppast::cpp_entity_kind::namespace_alias_t:
            return "Namespace alias";
        case cppast::cpp_entity_kind::using_directive_t:
            return "Using directive";
        case cppast::cpp_entity_kind::using_declaration_t:
            return "Using declaration";

        case cppast::cpp_entity_kind::type_alias_t:
            return "Type alias";

        case cppast::cpp_entity_kind::enum_t:
            return "Enumeration";
        case cppast::cpp_entity_kind::enum_value_t:
            return "Enumeration constant";

        case cppast::cpp_entity_kind::class_t:
            switch (static_cast<const cppast::cpp_class&>(e).class_kind())
            {
            case cppast::cpp_class_kind::class_t:
                return "Class";
            case cppast::cpp_class_kind::struct_t:
                return "Struct";
            case cppast::cpp_class_kind::union_t:
                return "Union";
            }
            break;
        case cppast::cpp_entity_kind::access_specifier_t:
            return "Access specifier";
        case cppast::cpp_entity_kind::base_class_t:
            return "Base class";

        case cppast::cpp_entity_kind::variable_t:
        case cppast::cpp_entity_kind::member_variable_t:
        case cppast::cpp_entity_kind::bitfield_t:
            return "Variable";

        case cppast::cpp_entity_kind::function_parameter_t:
            return "Parameter";
        case cppast::cpp_entity_kind::function_t:
        case cppast::cpp_entity_kind::member_function_t:
            // TODO: recognize (special) operators
            return "Function";
        case cppast::cpp_entity_kind::conversion_op_t:
            return "Conversion operator";
        case cppast::cpp_entity_kind::constructor_t:
            // TODO: recognize special constructors
            return "Constructor";
        case cppast::cpp_entity_kind::destructor_t:
            return "Destructor";
        case cppast::cpp_entity_kind::friend_t:
            return "Friend function";

        case cppast::cpp_entity_kind::template_type_parameter_t:
        case cppast::cpp_entity_kind::non_type_template_parameter_t:
        case cppast::cpp_entity_kind::template_template_parameter_t:
            return "Template parameter";

        case cppast::cpp_entity_kind::alias_template_t:
            return "Alias template";
        case cppast::cpp_entity_kind::variable_template_t:
            return "Variable template";

        case cppast::cpp_entity_kind::function_template_t:
        case cppast::cpp_entity_kind::function_template_specialization_t:
        case cppast::cpp_entity_kind::class_template_t:
        case cppast::cpp_entity_kind::class_template_specialization_t:
            return get_entity_kind_spelling(*static_cast<const cppast::cpp_template&>(e).begin());

        case cppast::cpp_entity_kind::static_assert_t:
            return "Static assertion";

        case cppast::cpp_entity_kind::unexposed_t:
            return "Unexposed entity";

        case cppast::cpp_entity_kind::count:
            break;
        }

        assert(false);
        return "should never get here";
    }

    markup::documentation_header get_header(
        const cppast::cpp_entity& e, type_safe::optional_ref<const comment::doc_comment> comment,
        std::string name)
    {
        markup::heading::builder builder{markup::block_id()};

        auto heading =
            comment.map([](const comment::doc_comment& c) { return type_safe::ref(c.metadata()); })
                .map([](const comment::metadata& metadata) { return metadata.group(); })
                .map([](const comment::member_group& group) { return group.heading(); });

        if (heading)
            builder.add_child(markup::text::build(heading.value()));
        else
        {
            auto spelling = get_entity_kind_spelling(e);
            builder.add_child(markup::text::build(spelling));
            builder.add_child(markup::text::build(" "));
            builder.add_child(markup::code::build(std::move(name)));
        }

        return markup::documentation_header(builder.finish(),
                                            comment ? comment.value().metadata().module() :
                                                      type_safe::nullopt);
    }

    bool empty_sections(type_safe::optional_ref<const comment::doc_comment> comment)
    {
        if (comment)
            return comment.value().sections().empty();
        else
            return true;
    }

    std::unique_ptr<markup::term_description_item> get_inline_doc(
        markup::block_id id, const cppast::cpp_entity& e,
        type_safe::optional_ref<const comment::doc_comment> comment)
    {
        if (comment && comment.value().brief_section())
        {
            auto term = markup::term::build(markup::code::build(get_entity_name(e)));

            markup::description::builder description;
            for (auto& phrasing : comment.value().brief_section().value())
                description.add_child(markup::clone(phrasing));

            return markup::term_description_item::build(std::move(id), std::move(term),
                                                        description.finish());
        }
        else
            return nullptr;
    }
}

std::unique_ptr<markup::documentation_entity> standardese::generate_documentation(
    const generation_config& gen_config, const synopsis_config& syn_config,
    const cppast::cpp_entity_index& index, const doc_entity& entity)
{
    return entity.do_generate_documentation(gen_config, syn_config, index, nullptr,
                                            generate_synopsis(syn_config, index, entity));
}

std::unique_ptr<markup::documentation_entity> doc_cpp_entity::do_generate_documentation(
    const generation_config& gen_config, const synopsis_config& syn_config,
    const cppast::cpp_entity_index&                     index,
    type_safe::optional_ref<detail::inline_entity_list> inlines,
    std::unique_ptr<markup::code_block>                 synopsis) const
{
    auto inline_doc =
        gen_config.is_flag_set(generation_config::inline_doc) && empty_sections(comment());

    if (group_member_no_.value_or(1u) != 1u || get_documentation_id().as_str() != link_name())
        // not a main entity that needs documentation
        return nullptr;
    // various inline entities
    else if (inline_doc && entity().kind() == cppast::cpp_function_parameter::kind())
        inlines.value().params.add_item(
            get_inline_doc(get_documentation_id(), entity(), comment()));
    else if (inline_doc && cppast::is_parameter(entity().kind()))
        // not a function parameter at this point
        inlines.value().tparams.add_item(
            get_inline_doc(get_documentation_id(), entity(), comment()));
    else if (inline_doc && entity().kind() == cppast::cpp_base_class::kind())
        inlines.value().bases.add_item(get_inline_doc(get_documentation_id(), entity(), comment()));
    else if (inline_doc && entity().kind() == cppast::cpp_enum_value::kind())
        inlines.value().enumerators.add_item(
            get_inline_doc(get_documentation_id(), entity(), comment()));
    else if (inline_doc
             && (entity().kind() == cppast::cpp_member_variable::kind()
                 || entity().kind() == cppast::cpp_bitfield::kind()))
        inlines.value().members.add_item(
            get_inline_doc(get_documentation_id(), entity(), comment()));
    // non-inline entity
    else
    {
        markup::entity_documentation::builder builder(entity_, get_documentation_id(),
                                                      get_header(*entity_, comment(),
                                                                 get_entity_name(*entity_)),
                                                      std::move(synopsis));
        if (comment())
            comment::set_sections(builder, comment().value());

        detail::inline_entity_list my_inlines(link_name());
        for (auto& child : *this)
        {
            auto child_doc =
                child.do_generate_documentation(gen_config, syn_config, index,
                                                type_safe::ref(my_inlines),
                                                generate_synopsis(syn_config, index, child));
            if (child_doc)
            {
                assert(child_doc->kind() == markup::entity_kind::entity_documentation);
                builder.add_child(std::unique_ptr<markup::entity_documentation>(
                    static_cast<markup::entity_documentation*>(child_doc.release())));
            }
        }

        // add inlines
        if (!my_inlines.tparams.empty())
            builder.add_section(markup::list_section::build(markup::section_type::invalid,
                                                            "Template parameters",
                                                            my_inlines.tparams.finish()));
        if (!my_inlines.params.empty())
            builder.add_section(markup::list_section::build(markup::section_type::invalid,
                                                            "Parameters",
                                                            my_inlines.params.finish()));
        if (!my_inlines.bases.empty())
            builder.add_section(markup::list_section::build(markup::section_type::invalid,
                                                            "Base classes",
                                                            my_inlines.bases.finish()));
        if (!my_inlines.enumerators.empty())
            builder.add_section(markup::list_section::build(markup::section_type::invalid,
                                                            "Enumerators",
                                                            my_inlines.enumerators.finish()));
        if (!my_inlines.members.empty())
            builder.add_section(markup::list_section::build(markup::section_type::invalid,
                                                            "Member variables",
                                                            my_inlines.members.finish()));

        if (comment() || (!builder.empty() && !builder.has_documentation())
            || gen_config.is_flag_set(generation_config::document_uncommented))
            return builder.finish();
    }

    return nullptr;
}

std::unique_ptr<markup::documentation_entity> doc_member_group_entity::do_generate_documentation(
    const generation_config& gen_config, const synopsis_config& syn_config,
    const cppast::cpp_entity_index&                     index,
    type_safe::optional_ref<detail::inline_entity_list> inlines,
    std::unique_ptr<markup::code_block>                 synopsis) const
{
    return begin()->do_generate_documentation(gen_config, syn_config, index, inlines,
                                              std::move(synopsis));
}

std::unique_ptr<markup::documentation_entity> doc_cpp_namespace::do_generate_documentation(
    const generation_config& gen_config, const synopsis_config& syn_config,
    const cppast::cpp_entity_index&     index, type_safe::optional_ref<detail::inline_entity_list>,
    std::unique_ptr<markup::code_block> synopsis) const
{
    // generate child documentation
    std::vector<std::unique_ptr<markup::entity_documentation>> child_docs;
    for (auto& child : *this)
    {
        auto child_doc =
            child.do_generate_documentation(gen_config, syn_config, index, nullptr,
                                            generate_synopsis(syn_config, index, child));
        if (child_doc)
        {
            assert(child_doc->kind() == markup::entity_kind::entity_documentation);
            child_docs.push_back(std::unique_ptr<markup::entity_documentation>(
                static_cast<markup::entity_documentation*>(child_doc.release())));
        }
    }

    if (child_docs.empty() && comment())
    {
        // generate documentation of namespace, if there is any
        markup::entity_documentation::builder builder(entity_, get_documentation_id(),
                                                      get_header(namespace_(), comment(),
                                                                 namespace_().name()),
                                                      std::move(synopsis));
        comment::set_sections(builder, comment().value());

        return builder.finish();
    }
    else
    {
        // generate empty namespace documentation
        markup::entity_documentation::builder builder(entity_, get_documentation_id(),
                                                      type_safe::nullopt, nullptr);
        for (auto& doc : child_docs)
            builder.add_child(std::move(doc));

        return builder.finish();
    }
}

markup::namespace_documentation::builder doc_cpp_namespace::get_builder() const
{
    markup::namespace_documentation::builder builder(entity_, get_documentation_id(),
                                                     get_header(namespace_(), comment(),
                                                                namespace_().name()));
    if (comment())
        comment::set_sections(builder, comment().value());
    return builder;
}

std::unique_ptr<markup::documentation_entity> doc_cpp_file::do_generate_documentation(
    const generation_config& gen_config, const synopsis_config& syn_config,
    const cppast::cpp_entity_index&     index, type_safe::optional_ref<detail::inline_entity_list>,
    std::unique_ptr<markup::code_block> synopsis) const
{
    markup::file_documentation::builder builder(type_safe::ref(*file_), get_documentation_id(),
                                                get_header(*file_, comment(), output_name()),
                                                std::move(synopsis));
    if (comment())
        comment::set_sections(builder, comment().value());

    for (auto& child : *this)
    {
        auto child_doc =
            child.do_generate_documentation(gen_config, syn_config, index, nullptr,
                                            generate_synopsis(syn_config, index, child));
        if (child_doc)
        {
            assert(child_doc->kind() == markup::entity_kind::entity_documentation);
            builder.add_child(std::unique_ptr<markup::entity_documentation>(
                static_cast<markup::entity_documentation*>(child_doc.release())));
        }
    }

    return builder.finish();
}

//=== entity builder ===//
doc_cpp_entity::builder::builder(std::string                                         link_name,
                                 type_safe::object_ref<const cppast::cpp_entity>     entity,
                                 type_safe::optional_ref<const comment::doc_comment> comment)
: basic_builder(std::unique_ptr<doc_cpp_entity>(
      new doc_cpp_entity(std::move(link_name), entity, std::move(comment))))
{
    assert(entity->kind() != cppast::cpp_file::kind()
           && entity->kind() != cppast::cpp_namespace::kind());
    peek().entity().set_user_data(&peek());
}

doc_cpp_namespace::builder::builder(std::string                                         link_name,
                                    type_safe::object_ref<const cppast::cpp_namespace>  entity,
                                    type_safe::optional_ref<const comment::doc_comment> comment)
: basic_builder(std::unique_ptr<doc_cpp_namespace>(
      new doc_cpp_namespace(std::move(link_name), entity, std::move(comment))))
{
    peek().namespace_().set_user_data(&peek());
}

doc_cpp_file::builder::builder(std::string output_name, std::string link_name,
                               std::unique_ptr<cppast::cpp_file>                   file,
                               type_safe::optional_ref<const comment::doc_comment> comment)
: basic_builder(
      std::unique_ptr<doc_cpp_file>(new doc_cpp_file(std::move(output_name), std::move(link_name),
                                                     std::move(file), std::move(comment))))
{
    peek().file().set_user_data(&peek());
}

namespace
{
    bool is_virtual(const cppast::cpp_entity& e)
    {
        if (auto func = detail::get_function(e))
        {
            if (func.value().kind() == cppast::cpp_member_function::kind()
                || func.value().kind() == cppast::cpp_conversion_op::kind())
                return static_cast<const cppast::cpp_member_function_base&>(func.value())
                    .is_virtual();
            else if (func.value().kind() == cppast::cpp_destructor::kind())
                return static_cast<const cppast::cpp_destructor&>(func.value()).is_virtual();
        }

        return false;
    }

    bool is_friend_func_def(const cppast::cpp_entity& e)
    {
        if (e.kind() == cppast::cpp_friend::kind())
        {
            auto& friend_ = static_cast<const cppast::cpp_friend&>(e);
            return friend_.entity() && cppast::is_definition(friend_.entity().value());
        }
        else
            return false;
    }
}

bool entity_blacklist::is_blacklisted(const cppast::cpp_entity&         entity,
                                      cppast::cpp_access_specifier_kind access) const
{
    if (!extract_private_ && access == cppast::cpp_private && !is_virtual(entity)
        && !is_friend_func_def(entity))
        return true;
    else if (entity.kind() == cppast::cpp_namespace::kind())
    {
        auto name = entity.name();
        if (ns_blacklist_.count(name))
            return true;

        for (auto cur = entity.parent(); cur; cur = cur.value().parent())
        {
            auto scope = cur.value().scope_name();
            if (scope)
            {
                name = scope.value().name() + "::" + name;
                if (ns_blacklist_.count(name))
                    return true;
            }
        }

        return false;
    }
    else
        return false;
}

namespace
{
    bool is_excluded(const cppast::cpp_entity& e, cppast::cpp_access_specifier_kind access,
                     type_safe::optional_ref<const comment::doc_comment> comment,
                     const entity_blacklist&                             blacklist)
    {
        if (blacklist.is_blacklisted(e, access))
            return true;
        else if (!comment
                 && (e.kind() == cppast::cpp_entity_kind::class_t
                     || e.kind() == cppast::cpp_entity_kind::class_template_t
                     || e.kind() == cppast::cpp_entity_kind::class_template_specialization_t
                     || e.kind() == cppast::cpp_entity_kind::class_template_specialization_t
                     || e.kind() == cppast::cpp_entity_kind::enum_t)
                 && !cppast::is_definition(e))
            // remove uncommented type forward declarations
            return true;
        else
            return comment && comment.value().metadata().exclude() == comment::exclude_mode::entity;
    }

    bool is_ignored(const cppast::cpp_entity& e)
    {
        return e.kind() == cppast::cpp_include_directive::kind()
               || e.kind() == cppast::cpp_using_declaration::kind()
               || e.kind() == cppast::cpp_using_directive::kind()
               || e.kind() == cppast::cpp_static_assert::kind()
               || e.kind() == cppast::cpp_access_specifier::kind()
               || e.kind() == cppast::cpp_language_linkage::kind();
    }

    std::unique_ptr<doc_entity> build_entity(const comment_registry&           registry,
                                             const cppast::cpp_entity&         e,
                                             cppast::cpp_access_specifier_kind access,
                                             const entity_blacklist&           blacklist);

    std::unique_ptr<doc_cpp_entity> build_cpp_entity(const comment_registry&   registry,
                                                     const cppast::cpp_entity& e,
                                                     const entity_blacklist&   blacklist)
    {
        doc_cpp_entity::builder builder(lookup_unique_name(registry, e), type_safe::ref(e),
                                        registry.get_comment(e));

        auto visitor = [&](const cppast::cpp_entity&         entity,
                           cppast::cpp_access_specifier_kind access = cppast::cpp_public) {
            if (auto child = build_entity(registry, entity, access, blacklist))
                builder.add_child(std::move(child));
        };

        // handle inline entities
        if (auto templ = detail::get_template(e))
            for (auto& param : templ.value().parameters())
                visitor(param);
        if (auto func = detail::get_function(e))
            for (auto& param : func.value().parameters())
                visitor(param);
        if (auto c = detail::get_class(e))
            for (auto& base : c.value().bases())
                visitor(base, base.access_specifier());

        detail::visit_children(e, visitor);

        return builder.finish();
    }

    std::unique_ptr<doc_member_group_entity> build_member_group(const comment_registry& registry,
                                                                const std::string&      group_name,
                                                                const cppast::cpp_entity& e,
                                                                const entity_blacklist&   blacklist)
    {
        auto group = registry.lookup_group(group_name);
        assert(group.size() >= 1u);
        if (&*group[0u] != &e)
            return nullptr;

        doc_member_group_entity::builder builder(group_name);
        for (auto& member : group)
            builder.add_member(build_cpp_entity(registry, *member, blacklist));
        return builder.finish();
    }

    std::unique_ptr<doc_cpp_namespace> build_namespace(const comment_registry&      registry,
                                                       const cppast::cpp_namespace& ns,
                                                       const entity_blacklist&      blacklist)
    {
        doc_cpp_namespace::builder builder(lookup_unique_name(registry, ns), type_safe::ref(ns),
                                           registry.get_comment(ns));

        detail::visit_children(ns, [&](const cppast::cpp_entity&         entity,
                                       cppast::cpp_access_specifier_kind access) {
            if (auto child = build_entity(registry, entity, access, blacklist))
                builder.add_child(std::move(child));
        });

        return builder.finish();
    }

    std::unique_ptr<doc_entity> build_entity(const comment_registry&           registry,
                                             const cppast::cpp_entity&         e,
                                             cppast::cpp_access_specifier_kind access,
                                             const entity_blacklist&           blacklist)
    {
        auto comment = registry.get_comment(e);
        if (is_excluded(e, access, comment, blacklist))
        {
            e.set_user_data(&excluded_entity);
            return nullptr;
        }
        else if (cppast::is_templated(e) || cppast::is_friended(e))
            // parent entity is processed here
            return nullptr;
        else if (is_ignored(e)
                 || (e.kind() == cppast::cpp_friend::kind() && !is_friend_func_def(e)))
            // those can't be documented
            return nullptr;
        else if (e.kind() == cppast::cpp_namespace::kind())
            return build_namespace(registry, static_cast<const cppast::cpp_namespace&>(e),
                                   blacklist);
        else if (comment.has_value() && comment.value().metadata().group())
            return build_member_group(registry, comment.value().metadata().group().value().name(),
                                      e, blacklist);
        else
            return build_cpp_entity(registry, e, blacklist);
    }
}

std::unique_ptr<doc_cpp_file> standardese::build_doc_entities(
    type_safe::object_ref<const comment_registry> registry, std::unique_ptr<cppast::cpp_file> file,
    std::string output_name, const entity_blacklist& blacklist)
{
    auto&                 f = *file;
    doc_cpp_file::builder builder(std::move(output_name), lookup_unique_name(*registry, f),
                                  std::move(file), registry->get_comment(f));

    detail::visit_children(f, [&](const cppast::cpp_entity&         entity,
                                  cppast::cpp_access_specifier_kind access) {
        if (auto child = build_entity(*registry, entity, access, blacklist))
            builder.add_child(std::move(child));
    });

    return builder.finish();
}
