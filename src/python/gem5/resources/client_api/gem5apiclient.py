# Copyright (c) 2023 The Regents of the University of California
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

from urllib import request, parse
from pathlib import Path
from typing import Optional, Dict, List, Any
import json
import os
import time
import itertools
from .abstract_client import AbstractClient
from m5.util import warn


class Gem5APIClientHttpJsonRequestError(Exception):
    def __init__(
        self,
        client: "Gem5APIClient",
        url: str,
        purpose_of_request: Optional[str],
        response: Optional[str] = None,
    ):
        """An exception raised when an HTTP request to the gem5 Resources API
        fails.
        :param client: The Gem5APIClient instance that raised the exception.
        :param url: The URL which was requested.
        :param purpose_of_request: A string describing the purpose of the
        request.
        :param response: The error returned by the server, if any.
        """
        error_str = (
            f"Http Request to the gem5 Resources API failed.\n"
            f"API URL: {client.url}\n"
            f"Requested URL: {url}\n"
            f"Response: {str(response)}\n"
        )

        if purpose_of_request:
            error_str += f"Purpose of Request: {purpose_of_request}\n"
        super().__init__(error_str)


class Gem5APIClient(AbstractClient):
    """
    A client for the gem5 Resources API (https://api.gem5.org/api/resources).

    This replaces the `AtlasClient`, which queried the MongoDB Atlas Data API.
    That API has been retired by MongoDB and now returns "HTTP Error 410:
    Gone" for every request.

    Every successful lookup is cached on the local disk. If the API cannot be
    reached (the host is offline, DNS fails, the service is down), the cache
    is consulted instead. As the resources themselves are also cached locally,
    this allows a simulation which has been run once to be re-run with no
    network connection at all.
    """

    def __init__(self, config: Dict[str, str]):
        """
        Initializes a connection to the gem5 resources database via the gem5
        Resources API.
        :param config: The source's config. Only the "url" field, the base URL
        of the API, is used.
        """
        self.url = config["url"]
        self.cache_file = Path(_get_metadata_cache_dir()).joinpath(
            "resource-metadata-cache.json"
        )

    def _read_cache(self) -> Dict[str, List[Dict[str, Any]]]:
        """Returns the on-disk metadata cache, keyed by resource ID. An
        unreadable or corrupt cache is treated as an empty one."""
        if not self.cache_file.is_file():
            return {}
        try:
            with open(self.cache_file) as f:
                return json.load(f)
        except (OSError, json.JSONDecodeError):
            return {}

    def _write_cache(
        self, resource_id: str, resources: List[Dict[str, Any]]
    ) -> None:
        """Records the API's answer for `resource_id` in the on-disk cache. A
        cache which cannot be written is not an error: it just means the next
        lookup goes to the API again."""
        cache = self._read_cache()
        cache[resource_id] = resources
        try:
            self.cache_file.parent.mkdir(parents=True, exist_ok=True)
            with open(self.cache_file, "w") as f:
                json.dump(cache, f, indent=4)
        except OSError as e:
            warn(f"Could not write to '{self.cache_file}': {e}")

    def _api_http_json_req(
        self,
        url: str,
        purpose_of_request: Optional[str],
        max_failed_attempts: int = 3,
        reattempt_pause_base: int = 2,
    ) -> Any:
        """Sends a GET request to the gem5 Resources API and returns the
        response. This function will attempt to reconnect to the server if the
        connection fails a set number of times before raising an exception.

        :param url: The URL to open the connection.
        :param purpose_of_request: A string describing the purpose of the
        request. This is optional. It's used to give context to the user if an
        exception is raised.
        :param max_failed_attempts: The maximum number of times an attempt at
        making a request should be done before throwing an exception.
        :param reattempt_pause_base: The base of the exponential backoff -- the
        time between each attempt.

        **Warning**: This function assumes a JSON response.
        """
        req = request.Request(url)

        for attempt in itertools.count(start=1):
            try:
                response = request.urlopen(req)
                break
            except Exception as e:
                if attempt >= max_failed_attempts:
                    raise Gem5APIClientHttpJsonRequestError(
                        client=self,
                        url=url,
                        purpose_of_request=purpose_of_request,
                        response=str(e),
                    )
                pause = reattempt_pause_base**attempt
                warn(
                    f"Attempt {attempt} of gem5 Resources API HTTP Request "
                    f"failed.\n"
                    f"Purpose of Request: {purpose_of_request}.\n\n"
                    f"Failed with Exception:\n{e}\n\n"
                    f"Retrying after {pause} seconds..."
                )
                time.sleep(pause)

        return json.loads(response.read().decode("utf-8"))

    def get_resources(
        self,
        resource_id: Optional[str] = None,
        resource_version: Optional[str] = None,
        gem5_version: Optional[str] = None,
    ) -> List[Dict[str, Any]]:
        if resource_id is None:
            # The API only exposes a look-up by ID. It cannot dump the whole
            # database, which is what a `None` `resource_id` asks for.
            warn(
                "The gem5 Resources API cannot list every resource. "
                "All resources may be browsed at https://resources.gem5.org."
            )
            return []

        # The API returns every version of a resource when the
        # "resource_version" parameter is the string "None".
        params = parse.urlencode(
            {
                "id": resource_id,
                "resource_version": (
                    resource_version if resource_version else "None"
                ),
            }
        )
        url = f"{self.url}/find-resources-in-batch?{params}"

        # A resource's metadata does not change once published, so when it is
        # already cached there is no point in a long retry loop: one failed
        # attempt is enough to fall back on.
        cached = self._read_cache().get(resource_id)

        try:
            resources = self._api_http_json_req(
                url,
                purpose_of_request="Get Resources",
                max_failed_attempts=1 if cached is not None else 3,
            )
            self._write_cache(resource_id, resources)
        except Gem5APIClientHttpJsonRequestError as e:
            # The API is unreachable. Fall back to the last answer it gave for
            # this resource, if there is one.
            if cached is None:
                raise e
            warn(
                f"The gem5 Resources API could not be reached. Using the "
                f"metadata cached in '{self.cache_file}' for resource "
                f"'{resource_id}'."
            )
            resources = cached

        return self.filter_incompatible_resources(
            resources_to_filter=resources, gem5_version=gem5_version
        )


def _get_metadata_cache_dir() -> str:
    """
    Returns the directory in which the resource metadata cache is kept. This
    is the same directory the resources themselves are downloaded to, so that
    clearing the one clears the other.

    Note: this mirrors `_get_default_resource_dir` in `resource.py`. It is
    duplicated here as importing that module from a client would be circular.
    """
    if "GEM5_RESOURCE_DIR" in os.environ:
        return os.environ["GEM5_RESOURCE_DIR"]
    return os.path.join(Path.home(), ".cache", "gem5")
