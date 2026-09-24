FROM ubuntu:24.04

ENV ALLSHARE_PATH=/allshare

ARG GID=999
ARG UID=999

# Create a non-root user
RUN groupadd -g ${GID} -r allshare && useradd -u ${UID} -r -g allshare -d ${ALLSHARE_PATH} -s /sbin/nologin allshare

# Create / Go to app directory
WORKDIR ${ALLSHARE_PATH}

# Download the release binary
ADD https://github.com/BreadEpic/AllShareStreaming/releases/latest/download/allshare-x86_64-unknown-linux-gnu.tar.gz /tmp/app.tar.gz

# Extract it, create server directory, and change ownership
RUN tar -xvzf /tmp/app.tar.gz --strip-components=1 -C ${ALLSHARE_PATH}/ && \
    rm /tmp/app.tar.gz && \
    mkdir -p ${ALLSHARE_PATH}/server && \
    chown -R allshare:allshare ${ALLSHARE_PATH} && \
    chmod +x ${ALLSHARE_PATH}/allshare

# Create a volume with the config.json and data.json
VOLUME ${ALLSHARE_PATH}/server

# Expose the port (adjust if you changed it)
EXPOSE 8080 40000-40100/udp

# Create an entrypoint
COPY entrypoint.sh /entrypoint.sh
RUN chown allshare:allshare /entrypoint.sh && \
    chmod +x /entrypoint.sh

# Switch to the non-root user
USER allshare

# Run the binary
ENTRYPOINT ["/entrypoint.sh"]
