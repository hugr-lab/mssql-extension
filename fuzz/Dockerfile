# Local deep-discovery image: AFL++ (CmpLog/RedQueen) + clang/LLVM, Linux.
# Multi-arch base — runs natively on arm64 (Apple Silicon) and amd64.
#
#   docker build -t mssql-fuzz -f fuzz/Dockerfile .
#   docker run --rm -it -v "$PWD/fuzz/findings:/repo/fuzz/findings" mssql-fuzz \
#          fuzz/run.sh tds_tokens
FROM aflplusplus/aflplusplus:latest

RUN apt-get update && apt-get install -y --no-install-recommends \
        curl unzip zip ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /repo
COPY . /repo

# AFL++ instrumentation everywhere; let the entrypoint pick the target.
ENV AFL_SKIP_CPUFREQ=1 \
    AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES=1 \
    AFL_TESTCACHE_SIZE=256

ENTRYPOINT ["/bin/bash"]
CMD ["fuzz/run.sh", "browser_response"]
