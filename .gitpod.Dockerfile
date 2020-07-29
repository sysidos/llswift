FROM gitpod/workspace-full

USER gitpod

RUN bash -c "$(wget -O - https://apt.llvm.org/llvm.sh)"
