# Optional container wrapper around the Linux x86_64 company pipeline entry point.
# Build context: repository root. Exports SDK artifacts; does not start an Agent.
ARG AME_BUILD_IMAGE=debian:bookworm-slim
FROM ${AME_BUILD_IMAGE} AS builder
ENV LANG=C.UTF-8 LC_ALL=C.UTF-8
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        cmake g++ make maven openjdk-17-jdk-headless python3 binutils ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY core/ core/
COPY storage/ storage/
COPY time/ time/
COPY vector/ vector/
COPY keyword/ keyword/
COPY extractor/ extractor/
COPY entity/ entity/
COPY graph_writer/ graph_writer/
COPY seed/ seed/
COPY diffusion/ diffusion/
COPY rerank/ rerank/
COPY engine/ engine/
COPY java/native/ java/native/
COPY java/scripts/ java/scripts/
COPY java/pom.xml java/pom.xml
COPY java/src/main/java/ java/src/main/java/
COPY java/src/main/resources/ java/src/main/resources/
COPY java/src/test/java/ java/src/test/java/
COPY .cloudbuild/ .cloudbuild/
RUN bash .cloudbuild/build.sh

FROM scratch AS artifact
COPY --from=builder /src/build/cloudbuild/artifacts/ /
