FROM gitpod/workspace-full

USER root
RUN bash -c "$(wget -O - https://apt.llvm.org/llvm.sh)"

USER gitpod
