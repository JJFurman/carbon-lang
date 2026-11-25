# Part of the Carbon Language project, under the Apache License v2.0 with LLVM
# Exceptions. See /LICENSE for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""Supports running a tool from the install filegroup."""

def _run_tool_impl(ctx):
    tool_files = ctx.attr.tool.files.to_list()
    if len(tool_files) != 1:
        fail("Expected 1 tool file, found {0}".format(len(tool_files)))
    tool_file = tool_files[0]
    ext = "." + tool_file.extension if tool_file.extension else ""
    out = ctx.actions.declare_file(ctx.label.name + ext)
    ctx.actions.symlink(
        output = out,
        target_file = tool_file,
        is_executable = True,
    )
    return [
        DefaultInfo(
            executable = out,
            files = depset([out]),
            runfiles = ctx.runfiles(files = ctx.files.data + [out]),
        ),
        RunEnvironmentInfo(
            environment = ctx.attr.env |
                          {"CARBON_ARGV0_OVERRIDE": tool_file.short_path},
        ),
    ]

run_tool = rule(
    doc = "Helper for running a tool in a filegroup.",
    implementation = _run_tool_impl,
    attrs = {
        "data": attr.label_list(allow_files = True),
        "env": attr.string_dict(),
        "tool": attr.label(
            allow_single_file = True,
            executable = True,
            cfg = "target",
            mandatory = True,
        ),
    },
    executable = True,
)
