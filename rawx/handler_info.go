// OpenIO SDS Go rawx
// Copyright (C) 2015-2020 OpenIO SAS
// Copyright (C) 2021-2024 OVH SAS
//
// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Affero General Public
// License as published by the Free Software Foundation; either
// version 3.0 of the License, or (at your option) any later version.
//
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public
// License along with this program. If not, see <http://www.gnu.org/licenses/>.

package main

import (
	"bytes"
	"net/http"
	"time"
)

func doGetInfo(rr *rawxRequest) {
	bb := bytes.Buffer{}
	bb.WriteString("namespace ")
	bb.WriteString(rr.rawx.ns)
	bb.WriteRune('\n')
	bb.WriteString("path ")
	bb.WriteString(rr.rawx.path)
	bb.WriteRune('\n')
	if rr.rawx.id != "" {
		bb.WriteString("service_id ")
		bb.WriteString(rr.rawx.id)
		bb.WriteRune('\n')
	}
	if rr.rawx.tlsUrl != "" {
		bb.WriteString("url_tls ")
		bb.WriteString(rr.rawx.tlsUrl)
		bb.WriteRune('\n')
	}

	rr.replyCode(http.StatusOK)
	rr.TTFB = time.Since(rr.startTime)
	rr.rep.Write(bb.Bytes())
}

func (rr *rawxRequest) serveInfo() {
	if err := rr.drain(); err != nil {
		rr.replyError("", err)
		return
	}

	var spent uint64
	var ttfb uint64
	if !rr.rawx.isIOok() {
		rr.replyIoError(rr.rawx)
	} else {
		switch rr.req.Method {
		case "GET", "HEAD":
			doGetInfo(rr)
		default:
			rr.replyCode(http.StatusMethodNotAllowed)
		}
	}
	spent, ttfb = IncrementStatReqInfo(rr)

	if isVerbose() {
		LogHttp(AccessLogEvent{
			Status:    rr.status,
			TimeSpent: spent,
			BytesIn:   rr.bytesIn,
			BytesOut:  rr.bytesOut,
			Method:    rr.req.Method,
			Local:     rr.req.Host,
			Peer:      rr.req.RemoteAddr,
			Path:      rr.req.URL.Path,
			ReqId:     rr.reqid,
			TLS:       rr.req.TLS != nil,
			TTFB:      ttfb,
		})
	}
}
