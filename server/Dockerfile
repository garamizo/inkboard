FROM python:3.12-slim
COPY --from=ghcr.io/astral-sh/uv:0.11.14 /uv /bin/uv
WORKDIR /app
ENV UV_COMPILE_BYTECODE=1 UV_LINK_MODE=copy
COPY pyproject.toml uv.lock ./
RUN uv sync --frozen --no-dev --no-install-project
COPY inkboard_server ./inkboard_server
RUN uv sync --frozen --no-dev
ENV INKBOARD_CACHE_DIR=/cache
# `just up` passes the repo's VERSION; the image has no repo to read it from.
ARG INKBOARD_VERSION=dev
ENV INKBOARD_VERSION=$INKBOARD_VERSION
RUN mkdir -p /cache && chown nobody /cache
VOLUME /cache
EXPOSE 8000
USER nobody
CMD ["/app/.venv/bin/uvicorn", "inkboard_server.main:app", "--host", "0.0.0.0", "--port", "8000", \
     "--no-access-log", "--no-proxy-headers"]
