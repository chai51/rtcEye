
const _sents = new Map();

function createRequest(method, data) {
    const request =
    {
        request: true,
        id: Math.round(Math.random() * 10000000),
        method: method,
        data: data || {}
    };

    return request;
}

function setResponse(object) {
    const message = {};

    if (typeof object !== 'object' || Array.isArray(object)) {
        console.error('parse() | not an object');

        return;
    }

    // Request.
    if (object.request) {
        message.request = true;

        if (typeof object.method !== 'string') {
            console.error('parse() | missing/invalid method field');

            return;
        }

        if (typeof object.id !== 'number') {
            console.error('parse() | missing/invalid id field');

            return;
        }

        message.id = object.id;
        message.method = object.method;
        message.data = object.data || {};
    }
    // Response.
    else if (object.response) {
        message.response = true;

        if (typeof object.id !== 'number') {
            console.error('parse() | missing/invalid id field');

            return;
        }

        message.id = object.id;

        // Success.
        if (object.ok) {
            message.ok = true;
            message.data = object.data || {};
        }
        // Error.
        else {
            message.ok = false;
            message.errorCode = object.errorCode;
            message.errorReason = object.errorReason;
        }
    }
    // Notification.
    else if (object.notification) {
        message.notification = true;

        if (typeof object.method !== 'string') {
            console.error('parse() | missing/invalid method field');

            return;
        }

        message.method = object.method;
        message.data = object.data || {};
    }
    // Invalid.
    else {
        console.error('parse() | missing request/response field');

        return;
    }

    _handleResponse(message);
}

function getResponse(request) {
    return new Promise((pResolve, pReject) => {

        const sent =
        {
            id: request.id,
            method: request.method,
            resolve: function (data2) {
                if (!_sents.delete(request.id))
                    return;

                pResolve(data2);
            },
            reject: function (error) {
                if (!_sents.delete(request.id))
                    return;

                pReject(error);
            },
            close: function () {
                pReject(new Error('peer closed'));
            }
        };

        // Add sent stuff to the map.
        _sents.set(request.id, sent);
    });
}

function _handleResponse(response) {
    const sent = _sents.get(response.id);

    if (!sent) {
        console.error(
            'received response does not match any sent request [id:%s]', response.id);

        return;
    }

    if (response.ok) {
        sent.resolve(response.data);
    }
    else {
        const error = new Error(response.errorReason);

        error.code = response.errorCode;
        sent.reject(error);
    }
}
