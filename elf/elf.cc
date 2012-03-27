#include "elf++.hh"

#include <cstring>

using namespace std;

ELFPP_BEGIN_NAMESPACE

template<template<typename E, byte_order Order> class Hdr>
void canon_hdr(Hdr<Elf64, byte_order::native> *out, const void *data,
               elfclass ei_class, elfdata ei_data)
{
        switch (ei_class) {
        case elfclass::_32:
                switch (ei_data) {
                case elfdata::lsb:
                        out->from(*(Hdr<Elf32, byte_order::lsb>*)data);
                        break;
                case elfdata::msb:
                        out->from(*(Hdr<Elf32, byte_order::msb>*)data);
                        break;
                }
        case elfclass::_64:
                switch (ei_data) {
                case elfdata::lsb:
                        out->from(*(Hdr<Elf64, byte_order::lsb>*)data);
                        break;
                case elfdata::msb:
                        out->from(*(Hdr<Elf64, byte_order::msb>*)data);
                        return;
                }
        }
}

//////////////////////////////////////////////////////////////////
// class file
//

struct file::impl
{
        impl(const shared_ptr<loader> &l)
                : l(l) { }

        const shared_ptr<loader> l;
        Ehdr<> hdr;
        vector<section> sections;
        //vector<segment> segments;

        section invalid_section;
};

file::file(const std::shared_ptr<loader> &l)
        : m(make_shared<impl>(l))
{
        // Read the first six bytes to check the magic number, ELF
        // class, and byte order.
        struct core_hdr
        {
                char ei_magic[4];
                elfclass ei_class;
                elfdata ei_data;
                unsigned char ei_version;
        } *core_hdr = (struct core_hdr*)l->load(0, sizeof *core_hdr);

        // Check basic header
        if (strncmp(core_hdr->ei_magic, "\x7f" "ELF", 4) != 0)
                throw format_error("bad ELF magic number");
        if (core_hdr->ei_version != 1)
                throw format_error("unknown ELF version");
        if (core_hdr->ei_class != elfclass::_32 &&
            core_hdr->ei_class != elfclass::_64)
                throw format_error("bad ELF class");
        if (core_hdr->ei_data != elfdata::lsb &&
            core_hdr->ei_data != elfdata::msb)
                throw format_error("bad ELF data order");

        // Read in the real header and canonicalize it
        size_t hdr_size = (core_hdr->ei_class == elfclass::_32 ?
                           sizeof(Ehdr<Elf32>) : sizeof(Ehdr<Elf64>));
        const void *hdr = l->load(0, hdr_size);
        canon_hdr(&m->hdr, hdr, core_hdr->ei_class, core_hdr->ei_data);

        // More checks
        if (m->hdr.version != 1)
                throw format_error("bad section ELF version");
        if (m->hdr.shnum && m->hdr.shstrndx >= m->hdr.shnum)
                throw format_error("bad section name string table index");

        // Load sections
        const void *sec_data = l->load(m->hdr.shoff,
                                       m->hdr.shentsize * m->hdr.shnum);
        for (unsigned i = 0; i < m->hdr.shnum; i++) {
                const void *sec = ((const char*)sec_data) + i * m->hdr.shentsize;
                m->sections.push_back(section(*this, sec));
        }
}

const Ehdr<> &
file::get_hdr() const
{
        return m->hdr;
}

shared_ptr<loader>
file::get_loader() const
{
        return m->l;
}

const std::vector<section> &
file::sections() const
{
        return m->sections;
}

const section &
file::get_section(const std::string &name) const
{
        for (auto &sec : sections())
                if (name == sec.get_name(nullptr))
                        return sec;
        return m->invalid_section;
}

const section &
file::get_section(unsigned index) const
{
        if (index >= sections().size())
                return m->invalid_section;
        return sections().at(index);
}

//////////////////////////////////////////////////////////////////
// class section
//

struct section::impl
{
        impl(const file &f)
                : f(f) { }

        const file f;
        Shdr<> hdr;
        const char *name;
        size_t name_len;
        const void *data;
};

section::section(const file &f, const void *hdr)
        : m(make_shared<impl>(f))
{
        canon_hdr(&m->hdr, hdr, f.get_hdr().ei_class, f.get_hdr().ei_data);
}

const Shdr<> &
section::get_hdr() const
{
        return m->hdr;
}

const char *
section::get_name(size_t *len_out) const
{
        // XXX Should the section name strtab be cached?
        if (!m->name)
                m->name = m->f.get_section(m->f.get_hdr().shstrndx)
                        .as_strtab().get(m->hdr.name, &m->name_len);
        if (len_out)
                *len_out = m->name_len;
        return m->name;
}

string
section::get_name() const
{
        return get_name(nullptr);
}

const void *
section::data() const
{
        if (m->hdr.type == sht::nobits)
                return nullptr;
        if (!m->data)
                m->data = m->f.get_loader()->load(m->hdr.offset, m->hdr.size);
        return m->data;
}

size_t
section::size() const
{
        return m->hdr.size;
}

strtab
section::as_strtab() const
{
        if (m->hdr.type != sht::strtab)
                throw section_type_mismatch("cannot use section as strtab");
        return strtab(m->f, data(), size());
}

//////////////////////////////////////////////////////////////////
// class strtab
//

struct strtab::impl
{
        impl(const file &f, const char *data, const char *end)
                : f(f), data(data), end(end) { }

        const file f;
        const char *data, *end;
};

strtab::strtab(file f, const void *data, size_t size)
        : m(make_shared<impl>(f, (const char*)data, (const char *)data + size))
{
}

const char *
strtab::get(Elf64::Off offset, size_t *len_out)
{
        const char *start = m->data + offset;

        if (start >= m->end)
                throw range_error("string offset " + to_string(offset) + " exceeds section size");

        // Find the null terminator
        const char *p = start;
        while (p < m->end && *p)
                p++;
        if (p == m->end)
                throw format_error("unterminated string");

        if (len_out)
                *len_out = p - start;
        return start;
}

std::string
strtab::get(Elf64::Off offset)
{
        return get(offset, nullptr);
}

ELFPP_END_NAMESPACE