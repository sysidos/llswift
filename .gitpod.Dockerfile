FROM gitpod/workspace-full

USER root
RUN apt-get install uuid-dev
RUN bash -c "$(wget -O - https://apt.llvm.org/llvm.sh)"

USER gitpod
