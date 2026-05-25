{{/*
Expand the name of the chart.
*/}}
{{- define "drogon-blog.name" -}}
{{- default .Chart.Name .Values.nameOverride | trunc 63 | trimSuffix "-" -}}
{{- end -}}

{{/*
Full release name: <release>-<chart>, truncated to 63 chars (k8s name limit).
fullnameOverride wins outright; nameOverride affects only the inner segment.
*/}}
{{- define "drogon-blog.fullname" -}}
{{- if .Values.fullnameOverride -}}
{{- .Values.fullnameOverride | trunc 63 | trimSuffix "-" -}}
{{- else -}}
{{- $name := default .Chart.Name .Values.nameOverride -}}
{{- if contains $name .Release.Name -}}
{{- .Release.Name | trunc 63 | trimSuffix "-" -}}
{{- else -}}
{{- printf "%s-%s" .Release.Name $name | trunc 63 | trimSuffix "-" -}}
{{- end -}}
{{- end -}}
{{- end -}}

{{/*
chart label — used in selectors so we can roll without breaking match.
*/}}
{{- define "drogon-blog.chart" -}}
{{- printf "%s-%s" .Chart.Name .Chart.Version | replace "+" "_" | trunc 63 | trimSuffix "-" -}}
{{- end -}}

{{/*
Common labels (applied to every object). Selector labels MUST be a
subset and are stable across upgrades — never put image tags or
versions in selector-side labels.
*/}}
{{- define "drogon-blog.labels" -}}
helm.sh/chart: {{ include "drogon-blog.chart" . }}
{{ include "drogon-blog.selectorLabels" . }}
app.kubernetes.io/version: {{ .Chart.AppVersion | quote }}
app.kubernetes.io/managed-by: {{ .Release.Service }}
{{- end -}}

{{- define "drogon-blog.selectorLabels" -}}
app.kubernetes.io/name: {{ include "drogon-blog.name" . }}
app.kubernetes.io/instance: {{ .Release.Name }}
{{- end -}}

{{/*
ServiceAccount name: explicit if provided, otherwise fullname when
create=true, fall back to default SA otherwise.
*/}}
{{- define "drogon-blog.serviceAccountName" -}}
{{- if .Values.serviceAccount.create -}}
{{- default (include "drogon-blog.fullname" .) .Values.serviceAccount.name -}}
{{- else -}}
{{- default "default" .Values.serviceAccount.name -}}
{{- end -}}
{{- end -}}

{{/*
Secret name for DB password — prefer the user-managed Secret when
provided, otherwise the chart-templated one.
*/}}
{{- define "drogon-blog.dbSecretName" -}}
{{- if .Values.database.existingSecret -}}
{{- .Values.database.existingSecret -}}
{{- else -}}
{{- printf "%s-db" (include "drogon-blog.fullname" .) -}}
{{- end -}}
{{- end -}}

{{- define "drogon-blog.dbSecretKey" -}}
{{- if .Values.database.existingSecret -}}
{{- default "password" .Values.database.existingSecretKey -}}
{{- else -}}
password
{{- end -}}
{{- end -}}

{{/*
SMTP secret picker — same shape as DB.
*/}}
{{- define "drogon-blog.smtpSecretName" -}}
{{- if .Values.smtp.existingSecret -}}
{{- .Values.smtp.existingSecret -}}
{{- else -}}
{{- printf "%s-smtp" (include "drogon-blog.fullname" .) -}}
{{- end -}}
{{- end -}}

{{- define "drogon-blog.smtpSecretKey" -}}
{{- if .Values.smtp.existingSecret -}}
{{- default "smtp_password" .Values.smtp.existingSecretKey -}}
{{- else -}}
smtp_password
{{- end -}}
{{- end -}}

{{/*
Image reference: repository + tag (defaults to chart appVersion).
*/}}
{{- define "drogon-blog.image" -}}
{{- $tag := default .Chart.AppVersion .Values.image.tag -}}
{{- printf "%s:%s" .Values.image.repository $tag -}}
{{- end -}}

{{/*
Upstream Postgres host: where SOMETHING (the app, or pgbouncer when
the sidecar is on) eventually connects. CNPG flips this to the
in-cluster Cluster's read-write Service automatically. Used by
pgbouncer's POSTGRESQL_HOST and by the app when no sidecar is on.
*/}}
{{- define "drogon-blog.upstreamDbHost" -}}
{{- if .Values.cnpg.enabled -}}
{{- printf "%s-cnpg-rw" (include "drogon-blog.fullname" .) -}}
{{- else -}}
{{- .Values.database.host -}}
{{- end -}}
{{- end -}}

{{/*
Host the APP container sees. With pgbouncer enabled, that's the
sidecar on loopback; without, it's the upstream host directly.
*/}}
{{- define "drogon-blog.appDbHost" -}}
{{- if .Values.pgbouncer.enabled -}}
127.0.0.1
{{- else -}}
{{- include "drogon-blog.upstreamDbHost" . -}}
{{- end -}}
{{- end -}}

{{- define "drogon-blog.appDbPort" -}}
{{- if .Values.pgbouncer.enabled -}}
6432
{{- else -}}
{{- .Values.database.port -}}
{{- end -}}
{{- end -}}
