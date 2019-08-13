// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package fragments

const SendEventInPlace = `
{{- define "SendEventInPlaceMethodSignature" -}}
Send{{ .Name }}Event(::zx::unowned_channel _chan, ::fidl::DecodedMessage<{{ .Name }}Response> params)
{{- end }}

{{- define "SendEventInPlaceMethodDefinition" }}
zx_status_t {{ .LLProps.InterfaceName }}::{{ template "SendEventInPlaceMethodSignature" . }} {
  params.message()->_hdr = {};
  params.message()->_hdr.ordinal = {{ .Ordinals.Write.Name }};
  return ::fidl::Write(zx::unowned_channel(_chan), std::move(params));
}
{{- end }}
`
